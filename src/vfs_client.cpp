#include "zifi/vfs_client.hpp"

#include <Arduino.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace zifi {
namespace {

constexpr uint32_t kNormalTimeoutMs = 5000;
constexpr uint32_t kFragmentTimeoutMs = 30000;
constexpr uint32_t kMutateTimeoutMs = 60000;
constexpr uint32_t kCloseTimeoutMs = 180000;

struct FatWindowSinkContext {
  FatAllocationCache* cache;
  uint32_t rawOffset;
};

}  // namespace

VfsClient::VfsClient(UartTransport& transport)
    : transport_(transport),
      lastError_{},
      writeBuffer_{},
      fragment_{},
      writeCount_(0),
      writeSequence_(0),
      readSequence_(0),
      fatSequence_(0),
      capabilities_(0),
      filexCapabilities_(0),
      lastStatus_(0),
      openMode_(0),
      randomOffset_(0),
      randomOffsetValid_(false),
      writeActive_(false),
      writeFailed_(false),
      fatCache_(),
      fsInfoCache_(),
      fsInfoValid_(false) {
  snprintf(lastError_, sizeof(lastError_), "none");
}

void VfsClient::beginFatCache(bool psramAvailable) {
  // Новый сеанс работает с той картой, которую увидит сам: и геометрия, и
  // счётчик свободных кластеров относятся к прежнему тому.
  fsInfoValid_ = false;
  fatCache_.begin(psramAvailable);
}

void VfsClient::setError(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(lastError_, sizeof(lastError_), format, arguments);
  va_end(arguments);
}

void VfsClient::handleUnexpected(void* context, const PacketView& packet) {
  auto* self = static_cast<VfsClient*>(context);
  // PING и RESET остаются доступны даже во время долгой операции SD. Другие
  // команды не исполняются повторно, потому что они могли бы изменить VFS
  // состояние, пока текущий запрос ещё использует UART и разборщик кадров.
  if (packet.command == kPing) {
    self->transport_.send(kReady);
  } else if (packet.command == kSysReset) {
    self->transport_.sendAck();
    self->transport_.flush();
    delay(100);
    ESP.restart();
  }
}

bool VfsClient::request(uint8_t command, const uint8_t* data, uint16_t length,
                        uint32_t timeoutMs, PacketView& response) {
  setError("none");
  if (!transport_.send(command, data, length)) {
    setError("uart-send-%02x", command);
    return false;
  }
  // Сеть продолжает исполняться независимо на ядре 0, поэтому функция
  // фонового обслуживания, необходимая однокорному варианту, здесь не нужна.
  if (!transport_.waitFor(command, timeoutMs, response, nullptr, nullptr,
                          handleUnexpected, this)) {
    setError("timeout-%02x", command);
    return false;
  }
  return true;
}

bool VfsClient::pathRequest(uint8_t command, const char* path,
                            uint32_t timeoutMs, PacketView& response) {
  if (path == nullptr) {
    setError("path-null");
    return false;
  }
  const size_t length = strlen(path) + 1;
  if (length > kMaxPayload) {
    setError("path-long");
    return false;
  }
  return request(command, reinterpret_cast<const uint8_t*>(path),
                 static_cast<uint16_t>(length), timeoutMs, response);
}

bool VfsClient::stat(const char* path, VfsEntry& entry) {
  PacketView response;
  if (!pathRequest(kVfsStat, path, kNormalTimeoutMs, response)) {
    return false;
  }
  if (response.length < 6 || response.data[0] != 0) {
    setError("stat-%u", response.length ? response.data[0] : 255);
    return false;
  }
  entry.isDirectory = response.data[1] != 0;
  entry.size = readLe32(response.data + 2);
  entry.name[0] = 0;
  return true;
}

bool VfsClient::openDirectory(const char* path) {
  PacketView response;
  if (!pathRequest(kVfsOpenDir, path, kNormalTimeoutMs, response)) {
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    setError("opendir-%u", response.length ? response.data[0] : 255);
    return false;
  }
  return true;
}

bool VfsClient::readDirectory(VfsEntry& entry, bool& atEnd) {
  atEnd = false;
  PacketView response;
  if (!request(kVfsReadDir, nullptr, 0, kNormalTimeoutMs, response)) {
    return false;
  }
  if (response.length < 6 || response.data[0] != 0) {
    atEnd = true;
    return true;
  }
  entry.isDirectory = response.data[1] != 0;
  entry.size = readLe32(response.data + 2);
  size_t nameLength = response.length - 6;
  if (nameLength >= sizeof(entry.name)) {
    nameLength = sizeof(entry.name) - 1;
  }
  memcpy(entry.name, response.data + 6, nameLength);
  entry.name[nameLength] = 0;
  // Wild Commander дополняет некоторые FAT-расширения пробелом. В SMB имя
  // должно совпадать с тем, которое Windows затем использует для CREATE.
  while (nameLength > 0 &&
         (entry.name[nameLength - 1] == 0 ||
          entry.name[nameLength - 1] == ' ')) {
    entry.name[--nameLength] = 0;
  }
  return true;
}

void VfsClient::resetWriteState(bool active) {
  writeCount_ = 0;
  writeSequence_ = 0;
  writeActive_ = active;
  writeFailed_ = false;
}

bool VfsClient::openFile(const char* path, bool forWrite) {
  return openFileMode(path, forWrite ? 1 : 0);
}

bool VfsClient::openFileAppend(const char* path) {
  // Режим 2 не удаляет существующий файл. Z80 снова находит его FAT-запись,
  // после чего следующие WRITE_WINDOW продолжают APPEND с прежнего конца.
  return openFileMode(path, 2);
}

bool VfsClient::openFileRandom(const char* path) {
  return openFileMode(path, 3);
}

bool VfsClient::openFileMode(const char* path, uint8_t mode) {
  if (path == nullptr) {
    setError("open-path-null");
    return false;
  }
  const size_t pathLength = strlen(path) + 1;
  if (pathLength + 1 > sizeof(fragment_)) {
    setError("open-path-long");
    return false;
  }
  const bool forWrite = mode == 1 || mode == 2;
  // Позиционный режим не использует legacy-накопитель, но разделяет его
  // признак ошибки. Сбрасываем состояние и для FILEX, чтобы отказ одной
  // записи не блокировал все последующие OPEN=3 до перезагрузки ESP.
  if (forWrite || mode == 3) {
    resetWriteState(false);
  }
  readSequence_ = 0;
  capabilities_ = 0;
  filexCapabilities_ = 0;
  lastStatus_ = 0;
  randomOffset_ = 0;
  randomOffsetValid_ = false;
  fragment_[0] = mode;
  memcpy(fragment_ + 1, path, pathLength);
  PacketView response;
  const uint32_t timeout = forWrite ? kMutateTimeoutMs : kNormalTimeoutMs;
  if (!request(kVfsOpen, fragment_, static_cast<uint16_t>(pathLength + 1),
               timeout, response)) {
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    lastStatus_ = response.length ? response.data[0] : 0xFF;
    setError("open-%u", response.length ? response.data[0] : 255);
    return false;
  }
  // Старый WMF отвечает одним байтом. Новый добавляет маску возможностей,
  // поэтому прошивка сохраняет безопасный fallback без угадывания версии.
  if (response.length >= 2) {
    capabilities_ = response.data[1];
  }
  if (response.length >= 3) {
    filexCapabilities_ = response.data[2];
  }
  if (mode == 3 && filexCapabilities_ == 0) {
    setError("open-filex-unsupported");
    lastStatus_ = 0x15;
    return false;
  }
  openMode_ = mode;
  randomOffsetValid_ = mode == 3;
  if (forWrite) {
    resetWriteState(true);
  }
  if (mode == 1) {
    // CREATE/REPLACE уже мог освободить прежнюю цепочку неизвестной длины.
    invalidateFsCache();
  }
  return true;
}

bool VfsClient::seekFile(uint32_t offset) {
  if (openMode_ != 3) {
    setError("seek-not-random");
    lastStatus_ = 0x15;
    return false;
  }
  // Пропуск SEEK был возможен, только пока обе стороны одинаково понимали, кто
  // двигает позицию файла: ESP прибавляла прочитанное к своему счётчику, а
  // плагин — к своему, вызовом Vfs_AddRandomCount. Стоит одной половине
  // перестать это делать, и позиции расходятся молча: ESP считает, что SEEK не
  // нужен, а Z80 читает с прежнего места. Экономия составляла десяток байт на
  // окно, цена ошибки — тихо неверные данные, поэтому позицию задаём всегда.
  uint8_t payload[4];
  writeLe32(payload, offset);
  PacketView response;
  if (!request(kVfsSeek, payload, sizeof(payload), kNormalTimeoutMs,
               response)) {
    lastStatus_ = 0xFF;
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    lastStatus_ = response.length ? response.data[0] : 0xFF;
    setError("seek-%u", lastStatus_);
    return false;
  }
  lastStatus_ = 0;
  randomOffset_ = offset;
  randomOffsetValid_ = true;
  return true;
}

bool VfsClient::readFile(uint8_t* output, size_t capacity, size_t wanted,
                         size_t& received) {
  received = 0;
  if (output == nullptr || capacity == 0) {
    setError("read-range");
    return false;
  }
  if (wanted > capacity) {
    wanted = capacity;
  }
  if (wanted > kBlockSize) {
    wanted = kBlockSize;
  }
  uint8_t payload[2];
  writeLe16(payload, static_cast<uint16_t>(wanted));
  PacketView response;
  if (!request(kVfsRead, payload, sizeof(payload), kNormalTimeoutMs, response)) {
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    lastStatus_ = response.length ? response.data[0] : 0xFF;
    setError("read-%u", response.length ? response.data[0] : 255);
    return false;
  }
  received = response.length - 1;
  if (received > capacity || received > wanted) {
    setError("read-overrun");
    received = 0;
    return false;
  }
  if (received != 0) {
    memcpy(output, response.data + 1, received);
  }
  return true;
}

bool VfsClient::readFileWindow(size_t wanted, WindowSink sink,
                               void* sinkContext, size_t& received) {
  received = 0;
  const size_t windowLimit = openMode_ == 3 ? kFilexTransferWindowSize
                                            : kTransferWindowSize;
  if (wanted == 0 || wanted > windowLimit || sink == nullptr) {
    setError("read-window-range");
    return false;
  }

  if ((capabilities_ & kCapabilityReadWindow) == 0) {
    // Совместимость со старым WMF: тот же запрос выполняется секторами по 512
    // байт. Новый плагин выбирается только по явно объявленной возможности.
    while (received < wanted) {
      const size_t remaining = wanted - received;
      const size_t partWanted = remaining < kBlockSize ? remaining : kBlockSize;
      size_t part = 0;
      if (!readFile(fragment_, sizeof(fragment_), partWanted, part)) {
        return false;
      }
      if (part == 0 || !sink(sinkContext, fragment_, part)) {
        setError(part == 0 ? "read-window-empty" : "read-window-sink");
        return false;
      }
      received += part;
      if (part < partWanted) {
        break;
      }
    }
    return true;
  }

  setError("none");
  uint8_t payload[3];
  payload[0] = readSequence_;
  writeLe16(payload + 1, static_cast<uint16_t>(wanted));
  if (!transport_.send(kVfsReadWindow, payload, sizeof(payload))) {
    setError("read-window-send");
    return false;
  }

  const uint8_t sequence = readSequence_;
  uint16_t total = 0;
  uint16_t crc = 0xFFFF;
  while (true) {
    PacketView response;
    if (!transport_.waitFor(kVfsReadWindow, kFragmentTimeoutMs, response,
                            nullptr, nullptr, handleUnexpected, this)) {
      setError("read-window-timeout");
      return false;
    }
    if (response.length == 1 && response.data[0] == 1) {
      lastStatus_ = 1;
      return true;
    }
    if (response.length < 1 || response.data[0] != 0) {
      lastStatus_ = response.length ? response.data[0] : 0xFF;
      setError("read-window-status-%u",
               response.length ? response.data[0] : 255);
      return false;
    }
    if (response.length <= kReadWindowHeader) {
      setError("read-window-len");
      return false;
    }

    const uint8_t flags = response.data[1];
    if ((flags & static_cast<uint8_t>(~(kWindowStart | kWindowEnd))) != 0 ||
        response.data[2] != sequence) {
      setError("read-window-header");
      return false;
    }
    const uint16_t offset = readLe16(response.data + 3);
    const uint16_t frameTotal = readLe16(response.data + 5);
    const size_t part = response.length - kReadWindowHeader;
    const bool isStart = (flags & kWindowStart) != 0;
    const bool isEnd = (flags & kWindowEnd) != 0;
    if (isStart != (received == 0) || offset != received || frameTotal == 0 ||
        frameTotal > wanted || offset > frameTotal ||
        (total != 0 && frameTotal != total) ||
        part > static_cast<size_t>(frameTotal - offset)) {
      setError("read-window-order");
      return false;
    }
    total = frameTotal;
    const size_t next = received + part;
    if ((isEnd && next != total) || (!isEnd && next >= total)) {
      setError("read-window-end");
      return false;
    }
    if (!sink(sinkContext, response.data + kReadWindowHeader, part)) {
      setError("read-window-sink");
      return false;
    }
    crc = crc16CcittFalseUpdate(
        crc, response.data + kReadWindowHeader, part);
    received = next;
    if (isEnd) {
      const uint16_t expected = readLe16(response.data + 7);
      if (crc != expected) {
        setError("read-window-crc");
        return false;
      }
      readSequence_ = static_cast<uint8_t>(sequence + 1);
      lastStatus_ = 0;
      if (openMode_ == 3 && randomOffsetValid_) {
        randomOffset_ += static_cast<uint32_t>(received);
      }
      return true;
    }
  }
}

bool VfsClient::readFatWindow(uint32_t rawOffset, size_t wanted,
                              WindowSink sink, void* sinkContext,
                              size_t& received) {
  received = 0;
  if (wanted == 0 || wanted > kFatTransferWindowSize ||
      (rawOffset & 511U) != 0 || (wanted & 511U) != 0 || sink == nullptr) {
    setError("fat-window-range");
    return false;
  }

  setError("none");
  uint8_t payload[7];
  payload[0] = fatSequence_;
  writeLe32(payload + 1, rawOffset);
  writeLe16(payload + 5, static_cast<uint16_t>(wanted));
  if (!transport_.send(kVfsFatWindow, payload, sizeof(payload))) {
    setError("fat-window-send");
    return false;
  }

  const uint8_t sequence = fatSequence_;
  const uint16_t mapBytes = static_cast<uint16_t>(wanted / 32U);
  uint16_t total = 0;
  uint16_t crc = 0xFFFF;
  while (true) {
    PacketView response;
    if (!transport_.waitFor(kVfsFatWindow, kFragmentTimeoutMs, response,
                            nullptr, nullptr, handleUnexpected, this)) {
      setError("fat-window-timeout");
      return false;
    }
    if (response.length < 1 || response.data[0] != 0) {
      lastStatus_ = response.length ? response.data[0] : 0xFF;
      setError("fat-window-status-%u",
               response.length ? response.data[0] : 255);
      return false;
    }
    if (response.length <= kReadWindowHeader) {
      setError("fat-window-len");
      return false;
    }

    const uint8_t flags = response.data[1];
    if ((flags & static_cast<uint8_t>(~(kWindowStart | kWindowEnd))) != 0 ||
        response.data[2] != sequence) {
      setError("fat-window-header");
      return false;
    }
    const uint16_t offset = readLe16(response.data + 3);
    const uint16_t frameTotal = readLe16(response.data + 5);
    const size_t part = response.length - kReadWindowHeader;
    const bool isStart = (flags & kWindowStart) != 0;
    const bool isEnd = (flags & kWindowEnd) != 0;
    if (isStart != (received == 0) || offset != received ||
        frameTotal != mapBytes || offset > frameTotal ||
        (total != 0 && frameTotal != total) ||
        part > static_cast<size_t>(frameTotal - offset)) {
      setError("fat-window-order");
      return false;
    }
    total = frameTotal;
    const size_t next = received + part;
    if ((isEnd && next != total) || (!isEnd && next >= total)) {
      setError("fat-window-end");
      return false;
    }
    if (!sink(sinkContext, response.data + kReadWindowHeader, part)) {
      setError("fat-window-sink");
      return false;
    }
    crc = crc16CcittFalseUpdate(crc,
                                response.data + kReadWindowHeader, part);
    received = next;
    if (isEnd) {
      if (crc != readLe16(response.data + 7)) {
        setError("fat-window-crc");
        return false;
      }
      fatSequence_ = static_cast<uint8_t>(sequence + 1);
      lastStatus_ = 0;
      return true;
    }
  }
}

bool VfsClient::ingestFatWindow(void* context, const uint8_t* data,
                                size_t length) {
  auto* sink = static_cast<FatWindowSinkContext*>(context);
  if (!sink->cache->ingestMap(sink->rawOffset, data, length)) {
    return false;
  }
  sink->rawOffset += static_cast<uint32_t>(length * 32U);
  return true;
}

bool VfsClient::buildFatCache(VfsFsInfo& info) {
  if (fatCache_.matches(info.totalClusters, info.fatSectors, info.serial,
                        info.activeFat)) {
    info.freeClusters = fatCache_.freeClusters();
    info.flags |= 0x04;
    return true;
  }
  if (info.bytesPerSector != 512 ||
      !fatCache_.prepare(info.totalClusters, info.fatSectors, info.serial,
                         info.activeFat)) {
    setError("fat-cache-memory");
    return false;
  }

  const uint64_t required =
      (static_cast<uint64_t>(info.totalClusters) + 2U) * 4U;
  const uint64_t rounded = (required + 511U) & ~511ULL;
  if (rounded > static_cast<uint64_t>(info.fatSectors) * 512U ||
      rounded > 0xFFFFFFFFULL) {
    fatCache_.invalidate();
    setError("fat-cache-geometry");
    return false;
  }

  uint32_t rawOffset = 0;
  while (rawOffset < rounded) {
    const uint32_t remaining = static_cast<uint32_t>(rounded) - rawOffset;
    const size_t wanted = remaining < kFatTransferWindowSize
                              ? remaining
                              : kFatTransferWindowSize;
    FatWindowSinkContext sink{&fatCache_, rawOffset};
    size_t received = 0;
    if (!readFatWindow(rawOffset, wanted, ingestFatWindow, &sink, received) ||
        received != wanted / 32U || sink.rawOffset != rawOffset + wanted) {
      fatCache_.invalidate();
      return false;
    }
    rawOffset += static_cast<uint32_t>(wanted);
  }
  if (!fatCache_.finish()) {
    setError("fat-cache-incomplete");
    return false;
  }
  info.freeClusters = fatCache_.freeClusters();
  info.flags |= 0x04;
  return true;
}

bool VfsClient::checkBlockAck(const PacketView& response, uint8_t sequence,
                              uint16_t accepted) {
  if (response.length < 4) {
    setError("block-ack-len");
    return false;
  }
  if (response.data[0] != 0) {
    lastStatus_ = response.data[0];
    setError("block-status-%u", response.data[0]);
    return false;
  }
  if (response.data[1] != sequence) {
    setError("block-seq-%u", response.data[1]);
    return false;
  }
  const uint16_t got = readLe16(response.data + 2);
  if (got != accepted) {
    setError("block-accepted-%u", got);
    return false;
  }
  return true;
}

bool VfsClient::flushWriteBlock(size_t rawLength) {
  if (rawLength == 0 || rawLength > kBlockSize) {
    setError("block-range");
    return false;
  }
  const uint8_t sequence = writeSequence_;
  const uint16_t crc = crc16CcittFalse(writeBuffer_, rawLength);
  // Ответ может потеряться уже после физического изменения FAT.
  fatCache_.invalidate();

  // Первый RAW-фрагмент: вид, номер, исходная/хранимая длина, CRC и данные.
  fragment_[0] = 0x00;
  fragment_[1] = sequence;
  writeLe16(fragment_ + 2, static_cast<uint16_t>(rawLength));
  writeLe16(fragment_ + 4, static_cast<uint16_t>(rawLength));
  writeLe16(fragment_ + 6, crc);
  size_t count = rawLength < kFirstData ? rawLength : kFirstData;
  memcpy(fragment_ + 8, writeBuffer_, count);

  PacketView response;
  uint16_t accepted = static_cast<uint16_t>(count);
  if (!request(kVfsBlock, fragment_, static_cast<uint16_t>(8 + count),
               kFragmentTimeoutMs, response) ||
      !checkBlockAck(response, sequence, accepted)) {
    return false;
  }

  size_t offset = count;
  while (offset < rawLength) {
    const size_t remaining = rawLength - offset;
    count = remaining < kContinueData ? remaining : kContinueData;
    fragment_[0] = 0x80;
    fragment_[1] = sequence;
    writeLe16(fragment_ + 2, static_cast<uint16_t>(offset));
    memcpy(fragment_ + 4, writeBuffer_ + offset, count);
    accepted = static_cast<uint16_t>(offset + count);
    if (!request(kVfsBlock, fragment_, static_cast<uint16_t>(4 + count),
                 kFragmentTimeoutMs, response) ||
        !checkBlockAck(response, sequence, accepted)) {
      return false;
    }
    offset += count;
  }
  writeSequence_ = static_cast<uint8_t>(sequence + 1);
  return true;
}

bool VfsClient::writeFile(const uint8_t* data, size_t length) {
  if (!writeActive_) {
    setError("write-not-open");
    return false;
  }
  if (writeFailed_) {
    return false;
  }
  if (data == nullptr && length != 0) {
    setError("write-null");
    writeFailed_ = true;
    return false;
  }

  size_t offset = 0;
  while (offset < length) {
    const size_t room = kBlockSize - writeCount_;
    const size_t remaining = length - offset;
    const size_t take = remaining < room ? remaining : room;
    memcpy(writeBuffer_ + writeCount_, data + offset, take);
    writeCount_ += take;
    offset += take;
    if (writeCount_ == kBlockSize) {
      if (!flushWriteBlock(kBlockSize)) {
        writeFailed_ = true;
        return false;
      }
      writeCount_ = 0;
    }
  }
  return true;
}

bool VfsClient::writeFileWindow(size_t length, WindowSource source,
                                void* sourceContext) {
  const bool randomWrite = openMode_ == 3;
  if (!writeActive_ && !randomWrite) {
    setError("write-not-open");
    return false;
  }
  if (writeFailed_) {
    return false;
  }
  const size_t windowLimit = randomWrite ? kFilexTransferWindowSize
                                         : kTransferWindowSize;
  if (length == 0 || length > windowLimit || source == nullptr) {
    setError("write-window-range");
    writeFailed_ = true;
    return false;
  }

  if ((capabilities_ & kCapabilityWriteWindow) == 0) {
    if (randomWrite) {
      setError("write-window-unsupported");
      writeFailed_ = true;
      return false;
    }
    // Старый протокол остаётся рабочим: окно читается из PSRAM небольшими
    // кусками, а подтверждение выполняется по прежним блокам 512 байт.
    size_t offset = 0;
    while (offset < length) {
      const size_t remaining = length - offset;
      const size_t wanted = remaining < sizeof(fragment_)
                                ? remaining
                                : sizeof(fragment_);
      const size_t count = source(sourceContext, offset, fragment_, wanted);
      if (count != wanted || !writeFile(fragment_, count)) {
        if (count != wanted) {
          setError("write-window-source");
        }
        writeFailed_ = true;
        return false;
      }
      offset += count;
    }
    return true;
  }

  setError("none");
  // После первого кадра нельзя считать прежнюю карту гарантированно точной:
  // Z80 мог завершить commit, даже если ответ затем потеряется.
  fatCache_.invalidate();
  const uint8_t sequence = writeSequence_;
  uint16_t crc = 0xFFFF;
  size_t offset = 0;
  while (offset < length) {
    const size_t remaining = length - offset;
    const size_t count = remaining < kWriteWindowData
                             ? remaining
                             : kWriteWindowData;
    const bool isStart = offset == 0;
    const bool isEnd = offset + count == length;
    fragment_[0] = static_cast<uint8_t>((isStart ? kWindowStart : 0) |
                                        (isEnd ? kWindowEnd : 0));
    fragment_[1] = sequence;
    writeLe16(fragment_ + 2, static_cast<uint16_t>(offset));
    writeLe16(fragment_ + 4, static_cast<uint16_t>(length));
    writeLe16(fragment_ + 6, 0);
    if (source(sourceContext, offset, fragment_ + kWriteWindowHeader,
               count) != count) {
      setError("write-window-source");
      writeFailed_ = true;
      return false;
    }
    crc = crc16CcittFalseUpdate(
        crc, fragment_ + kWriteWindowHeader, count);
    if (isEnd) {
      writeLe16(fragment_ + 6, crc);
    }
    if (!transport_.send(
            kVfsWriteWindow, fragment_,
            static_cast<uint16_t>(kWriteWindowHeader + count))) {
      setError("write-window-send");
      writeFailed_ = true;
      return false;
    }
    offset += count;
  }

  // Все промежуточные кадры идут без ответа. Z80 подтверждает только целое
  // окно после проверки CRC и, для полных 16 КиБ, после завершения APPEND.
  PacketView response;
  if (!transport_.waitFor(kVfsWriteWindow, kCloseTimeoutMs, response,
                          nullptr, nullptr, handleUnexpected, this) ||
      !checkBlockAck(response, sequence, static_cast<uint16_t>(length))) {
    if (strcmp(lastError_, "none") == 0) {
      setError("write-window-timeout");
    }
    writeFailed_ = true;
    return false;
  }
  writeSequence_ = static_cast<uint8_t>(sequence + 1);
  lastStatus_ = 0;
  if (randomWrite && randomOffsetValid_) {
    randomOffset_ += static_cast<uint32_t>(length);
  }
  return true;
}

bool VfsClient::extendFile(uint32_t length) {
  if (!writeActive_) {
    setError("extend-not-open");
    return false;
  }
  if (writeFailed_) {
    return false;
  }
  if (length == 0) {
    return true;
  }
  if ((capabilities_ & kCapabilityExtend) == 0) {
    setError("extend-unsupported");
    return false;
  }
  // EXTEND открывается отдельной append-сессией. Накопленный legacy-хвост
  // нельзя заменять нулями до его публикации через CLOSE.
  if (writeCount_ != 0) {
    setError("extend-buffered");
    writeFailed_ = true;
    return false;
  }

  uint8_t payload[4];
  writeLe32(payload, length);
  fatCache_.invalidate();
  PacketView response;
  if (!request(kVfsExtend, payload, sizeof(payload), kCloseTimeoutMs,
               response)) {
    writeFailed_ = true;
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    setError("extend-%u", response.length ? response.data[0] : 255);
    writeFailed_ = true;
    return false;
  }
  return true;
}

bool VfsClient::setFileSize(uint32_t size, uint32_t& actualSize) {
  actualSize = 0;
  if (openMode_ != 3) {
    setError("set-eof-not-random");
    lastStatus_ = 0x15;
    return false;
  }
  uint8_t payload[4];
  writeLe32(payload, size);
  fatCache_.invalidate();
  PacketView response;
  if (!request(kVfsSetEof, payload, sizeof(payload), kMutateTimeoutMs,
               response)) {
    lastStatus_ = 0xFF;
    return false;
  }
  if (response.length < 1 ||
      (response.data[0] != 0 && response.data[0] != 0x25)) {
    lastStatus_ = response.length ? response.data[0] : 0xFF;
    setError("set-eof-%u", lastStatus_);
    return false;
  }
  if (response.length < 5) {
    setError("set-eof-short");
    lastStatus_ = 0xFF;
    return false;
  }
  actualSize = readLe32(response.data + 1);
  lastStatus_ = response.data[0];
  return true;
}

bool VfsClient::getFsInfo(VfsFsInfo& info, bool refreshFree, bool cacheOnly) {
  // Этот клиент — единственный владелец сведений о томе. Пока геометрия
  // прочитана, а число свободных кластеров поддерживается точным, ответ
  // отдаётся без обращения к Z80: иначе каждый опрос свободного места стоил
  // бы UART-обмена, а при неизвестном Free_Count — ещё и обхода всей FAT.
  if (fsInfoValid_) {
    info = fsInfoCache_;
    if (fatCache_.matches(info.totalClusters, info.fatSectors, info.serial,
                          info.activeFat)) {
      info.freeClusters = fatCache_.freeClusters();
      info.flags |= 0x04;
    }
    if ((info.flags & 0x04) != 0 || !refreshFree) {
      return true;
    }
  }
  if (cacheOnly) {
    // Вызывающий просил ответ без побочных эффектов: он ещё не закрывал
    // активный файл и по этому отказу поймёт, что закрыть придётся.
    setError("fs-info-miss");
    lastStatus_ = 0;
    return false;
  }
  // FILEX сначала отдаёт дешёвую геометрию и проверенный FSInfo. Если
  // Free_Count неизвестен, точная карта строится отдельным потоковым чтением
  // выбранной активной FAT и остаётся в PSRAM ESP.
  const uint8_t flags = 0;
  PacketView response;
  if (!request(kVfsFsInfo, &flags, 1, kMutateTimeoutMs, response)) {
    lastStatus_ = 0xFF;
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    lastStatus_ = response.length ? response.data[0] : 0xFF;
    setError("fs-info-%u", lastStatus_);
    return false;
  }
  if (response.length < 49 || response.data[1] < 48 || response.data[2] != 1) {
    setError("fs-info-short");
    lastStatus_ = 0xFF;
    return false;
  }
  const uint8_t* data = response.data + 1;
  info.flags = data[2];
  info.sectorsPerCluster = data[3];
  info.bytesPerSector = readLe16(data + 4);
  info.totalClusters = readLe32(data + 8);
  info.freeClusters = readLe32(data + 12);
  info.serial = readLe32(data + 16);
  memcpy(info.label, data + 20, 11);
  info.label[11] = 0;
  info.totalSectors = readLe32(data + 32);
  info.nextFree = readLe32(data + 36);
  info.fatCount = data[40];
  info.activeFat = data[41];
  info.fatFlags = data[42];
  info.mediaState = data[43];
  info.fatSectors = readLe32(data + 44);
  lastStatus_ = 0;
  if ((info.flags & 0x04) != 0 && info.freeClusters <= info.totalClusters) {
    rememberFsInfo(info);
    return true;
  }
  info.flags &= static_cast<uint8_t>(~0x04U);
  info.freeClusters = 0xFFFFFFFFUL;
  if (fatCache_.matches(info.totalClusters, info.fatSectors, info.serial,
                        info.activeFat)) {
    info.freeClusters = fatCache_.freeClusters();
    info.flags |= 0x04;
    rememberFsInfo(info);
    return true;
  }
  if (!refreshFree) {
    rememberFsInfo(info);
    return true;
  }
  if (!buildFatCache(info)) {
    return false;
  }
  rememberFsInfo(info);
  return true;
}

void VfsClient::rememberFsInfo(const VfsFsInfo& info) {
  fsInfoCache_ = info;
  fsInfoValid_ = true;
}

void VfsClient::noteFileResized(uint32_t oldSize, uint32_t newSize) {
  // Дельту размера знает только вызывающий: FILEX не сообщает, какие именно
  // кластеры выделил Wild Commander. Счётчик при этом остаётся здесь — он
  // единственный источник ответа о свободном месте.
  const uint32_t bytesPerCluster =
      static_cast<uint32_t>(fsInfoCache_.bytesPerSector) *
      fsInfoCache_.sectorsPerCluster;
  if (!fsInfoValid_ || bytesPerCluster == 0) {
    invalidateFsCache();
    return;
  }
  fatCache_.noteResize(oldSize, newSize, bytesPerCluster);
  if (fatCache_.matches(fsInfoCache_.totalClusters, fsInfoCache_.fatSectors,
                        fsInfoCache_.serial, fsInfoCache_.activeFat)) {
    fsInfoCache_.freeClusters = fatCache_.freeClusters();
    fsInfoCache_.flags |= 0x04;
    return;
  }
  // Карты нет — свободное место считает сама FAT32 через Free_Count.
  if ((fsInfoCache_.flags & 0x04) == 0) {
    return;
  }
  const uint32_t before =
      static_cast<uint32_t>((static_cast<uint64_t>(oldSize) +
                             bytesPerCluster - 1U) / bytesPerCluster);
  const uint32_t after =
      static_cast<uint32_t>((static_cast<uint64_t>(newSize) +
                             bytesPerCluster - 1U) / bytesPerCluster);
  if (after > before) {
    const uint32_t taken = after - before;
    fsInfoCache_.freeClusters = fsInfoCache_.freeClusters > taken
                                    ? fsInfoCache_.freeClusters - taken
                                    : 0;
  } else if (before > after) {
    const uint32_t released = before - after;
    const uint32_t free = fsInfoCache_.freeClusters + released;
    fsInfoCache_.freeClusters =
        free < fsInfoCache_.freeClusters || free > fsInfoCache_.totalClusters
            ? fsInfoCache_.totalClusters
            : free;
  }
}

void VfsClient::invalidateFsCache() {
  fsInfoValid_ = false;
  fatCache_.invalidate();
}

bool VfsClient::moveRename(const char* oldPath, const char* newPath,
                           bool directory, bool replace) {
  if (oldPath == nullptr || newPath == nullptr || *oldPath == 0 ||
      *newPath == 0) {
    setError("move-path-null");
    return false;
  }
  const size_t oldLength = strlen(oldPath) + 1;
  const size_t newLength = strlen(newPath) + 1;
  if (2 + oldLength + newLength > sizeof(fragment_)) {
    setError("move-path-long");
    return false;
  }
  fragment_[0] = replace ? 1 : 0;
  fragment_[1] = directory ? 0x10 : 0;
  memcpy(fragment_ + 2, oldPath, oldLength);
  memcpy(fragment_ + 2 + oldLength, newPath, newLength);
  // Замена существующего назначения освобождает его цепочку неизвестной длины.
  invalidateFsCache();
  PacketView response;
  if (!request(kVfsMoveRename, fragment_,
               static_cast<uint16_t>(2 + oldLength + newLength),
               kMutateTimeoutMs, response)) {
    lastStatus_ = 0xFF;
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    lastStatus_ = response.length ? response.data[0] : 0xFF;
    setError("move-%u", lastStatus_);
    return false;
  }
  lastStatus_ = 0;
  return true;
}

bool VfsClient::setMetadata(const VfsMetadata& metadata,
                            uint8_t& appliedAttributes) {
  if (openMode_ != 3) {
    setError("metadata-not-random");
    lastStatus_ = 0x15;
    return false;
  }
  uint8_t payload[16] = {};
  payload[0] = sizeof(payload);
  payload[1] = metadata.attrMask;
  payload[2] = metadata.attrValue;
  payload[3] = metadata.timeMask;
  payload[4] = metadata.createTenth;
  writeLe16(payload + 5, metadata.createTime);
  writeLe16(payload + 7, metadata.createDate);
  writeLe16(payload + 9, metadata.accessDate);
  writeLe16(payload + 11, metadata.writeTime);
  writeLe16(payload + 13, metadata.writeDate);
  PacketView response;
  if (!request(kVfsSetMetadata, payload, sizeof(payload), kMutateTimeoutMs,
               response)) {
    lastStatus_ = 0xFF;
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    lastStatus_ = response.length ? response.data[0] : 0xFF;
    setError("metadata-%u", lastStatus_);
    return false;
  }
  if (response.length < 2) {
    setError("metadata-short");
    lastStatus_ = 0xFF;
    return false;
  }
  appliedAttributes = response.data[1];
  lastStatus_ = 0;
  return true;
}

bool VfsClient::closeFile(bool commit) {
  const bool wasWrite = writeActive_;
  if (wasWrite && commit && writeFailed_) {
    char saved[sizeof(lastError_)];
    snprintf(saved, sizeof(saved), "%s", lastError_);
    const uint8_t abort = 0;
    PacketView ignored;
    request(kVfsClose, &abort, 1, kCloseTimeoutMs, ignored);
    resetWriteState(false);
    snprintf(lastError_, sizeof(lastError_), "%s", saved);
    return false;
  }

  if (wasWrite && commit && writeCount_ != 0) {
    if (!flushWriteBlock(writeCount_)) {
      char saved[sizeof(lastError_)];
      snprintf(saved, sizeof(saved), "%s", lastError_);
      const uint8_t abort = 0;
      PacketView ignored;
      request(kVfsClose, &abort, 1, kCloseTimeoutMs, ignored);
      resetWriteState(false);
      snprintf(lastError_, sizeof(lastError_), "%s", saved);
      return false;
    }
  }

  const uint8_t abort = 0;
  PacketView response;
  const uint8_t* payload = commit ? nullptr : &abort;
  const uint16_t length = commit ? 0 : 1;
  const bool requested = request(kVfsClose, payload, length,
                                 kCloseTimeoutMs, response);
  bool closed = requested && response.length >= 1 && response.data[0] == 0;
  if (requested && !closed) {
    setError("close-%u", response.length ? response.data[0] : 255);
  }
  resetWriteState(false);
  capabilities_ = 0;
  filexCapabilities_ = 0;
  openMode_ = 0;
  randomOffsetValid_ = false;
  if (requested) {
    lastStatus_ = response.length ? response.data[0] : 0xFF;
  }
  return closed;
}

bool VfsClient::remove(const char* path) {
  fatCache_.invalidate();
  PacketView response;
  if (!pathRequest(kVfsDelete, path, kMutateTimeoutMs, response)) {
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    setError("delete-%u", response.length ? response.data[0] : 255);
    return false;
  }
  return true;
}

bool VfsClient::makeDirectory(const char* path) {
  // Новый каталог занимает кластеры под собственные записи; сколько именно —
  // знает только FAT.
  invalidateFsCache();
  PacketView response;
  if (!pathRequest(kVfsMkdir, path, kMutateTimeoutMs, response)) {
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    setError("mkdir-%u", response.length ? response.data[0] : 255);
    return false;
  }
  return true;
}

bool VfsClient::rename(const char* oldPath, const char* newName,
                       bool directory) {
  if (oldPath == nullptr || newName == nullptr || *newName == 0) {
    setError("rename-path-null");
    return false;
  }
  const size_t oldLength = strlen(oldPath) + 1;
  const size_t newLength = strlen(newName) + 1;
  // Тело: атрибут FAT, старый полный путь и новое имя без каталога.
  // Каталоги получают стандартный атрибут #10, обычные файлы — ноль.
  if (1 + oldLength + newLength > sizeof(fragment_)) {
    setError("rename-path-long");
    return false;
  }
  fragment_[0] = directory ? 0x10 : 0x00;
  memcpy(fragment_ + 1, oldPath, oldLength);
  memcpy(fragment_ + 1 + oldLength, newName, newLength);

  PacketView response;
  if (!request(kVfsRename, fragment_,
               static_cast<uint16_t>(1 + oldLength + newLength),
               kMutateTimeoutMs, response)) {
    return false;
  }
  if (response.length < 1 || response.data[0] != 0) {
    setError("rename-%u", response.length ? response.data[0] : 255);
    return false;
  }
  return true;
}

}  // namespace zifi
