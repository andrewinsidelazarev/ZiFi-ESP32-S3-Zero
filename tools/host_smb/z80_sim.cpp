// Эмулятор Wild Commander на стороне Z80 для нативной сборки сервера.
//
// Он подключается к HardwareSerial-заглушке и говорит ровно тем же двоичным
// протоколом, что и настоящий плагин: [SYNC=0x5A][CMD][LEN_L][LEN_H][DATA][CSUM],
// где CSUM — XOR байта команды со всем, что за ним следует. Файловые операции
// выполняются поверх обычной папки Windows.
//
// Смысл существования: на железе одна итерация отладки стоит перепрошивки, а
// паника ESP не оставляет ни места падения, ни стека — только reset_reason=4.
// Здесь тот же самый серверный код падает под отладчиком с полным backtrace.

#include "z80_sim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace zifi {
namespace host {
namespace {

constexpr uint8_t kSync = 0x5A;

// Коды команд повторяют include/zifi/protocol.hpp. Дублируются намеренно:
// эмулятор обязан оставаться независимым от заголовков прошивки, иначе он
// начнёт «подстраиваться» под её ошибки вместо того, чтобы их ловить.
constexpr uint8_t kVfsStat = 0x40;
constexpr uint8_t kVfsOpenDir = 0x41;
constexpr uint8_t kVfsReadDir = 0x42;
constexpr uint8_t kVfsFsInfo = 0x43;
constexpr uint8_t kVfsOpen = 0x50;
constexpr uint8_t kVfsClose = 0x53;
constexpr uint8_t kVfsDelete = 0x54;
constexpr uint8_t kVfsMkdir = 0x55;
constexpr uint8_t kVfsWriteWindow = 0x57;
constexpr uint8_t kVfsReadWindow = 0x58;
constexpr uint8_t kVfsSeek = 0x5B;
constexpr uint8_t kVfsSetEof = 0x5C;

// Окно позиционного тракта: 16 КиБ минус 32 байта под блок параметров FILEX.
constexpr size_t kFilexWindow = 16 * 1024 - 32;
// Последовательное чтение идёт мимо FILEX и ограничено полными 16 КиБ.
constexpr size_t kSequentialWindow = 16 * 1024;
// Заголовок кадра чтения: [status][flags][seq][offset:2][total:2][crc16:2].
constexpr size_t kReadWindowHeader = 9;
constexpr size_t kWriteWindowHeader = 8;
constexpr size_t kMaxPayload = 1024;
constexpr uint8_t kWindowStart = 0x01;
constexpr uint8_t kWindowEnd = 0x02;

constexpr uint8_t kStatusOk = 0;
constexpr uint8_t kStatusFail = 1;

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

void writeLe16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}

// CRC-16/CCITT-FALSE — тот же полином и начальное значение, что у плагина.
// Совпадение обязательно: иначе сервер отвергнет каждое окно.
uint16_t crc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t index = 0; index < length; ++index) {
    crc ^= static_cast<uint16_t>(data[index]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

void writeLe32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace

Z80Simulator::Z80Simulator(HardwareSerial& serial, const std::string& root)
    : serial_(serial), root_(root) {
  serial_.attachSink(&Z80Simulator::sinkThunk, this);
}

void Z80Simulator::sinkThunk(void* context, const uint8_t* data,
                             size_t length) {
  static_cast<Z80Simulator*>(context)->consume(data, length);
}

// Разбор потока от сервера. Кадр отдаётся обработчику только целиком и только
// с верной контрольной суммой — как это делает настоящий плагин.
void Z80Simulator::consume(const uint8_t* data, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = data[index];
    switch (state_) {
      case State::kSync:
        if (value == kSync) {
          state_ = State::kCommand;
        }
        break;
      case State::kCommand:
        command_ = value;
        checksum_ = value;
        state_ = State::kLengthLow;
        break;
      case State::kLengthLow:
        expected_ = value;
        checksum_ ^= value;
        state_ = State::kLengthHigh;
        break;
      case State::kLengthHigh:
        expected_ |= static_cast<uint16_t>(value) << 8;
        checksum_ ^= value;
        payload_.clear();
        state_ = expected_ == 0 ? State::kChecksum : State::kPayload;
        break;
      case State::kPayload:
        payload_.push_back(value);
        checksum_ ^= value;
        if (payload_.size() == expected_) {
          state_ = State::kChecksum;
        }
        break;
      case State::kChecksum:
        if (value == checksum_) {
          handle(command_, payload_);
        }
        state_ = State::kSync;
        break;
    }
  }
}

void Z80Simulator::reply(uint8_t command, const uint8_t* data, size_t length) {
  std::vector<uint8_t> frame;
  frame.reserve(length + 6);
  frame.push_back(kSync);
  uint8_t checksum = command;
  frame.push_back(command);
  const uint8_t lengthLow = static_cast<uint8_t>(length);
  const uint8_t lengthHigh = static_cast<uint8_t>(length >> 8);
  frame.push_back(lengthLow);
  checksum ^= lengthLow;
  frame.push_back(lengthHigh);
  checksum ^= lengthHigh;
  for (size_t index = 0; index < length; ++index) {
    frame.push_back(data[index]);
    checksum ^= data[index];
  }
  frame.push_back(checksum);
  serial_.pushRx(frame.data(), frame.size());
}

void Z80Simulator::replyStatus(uint8_t command, uint8_t status) {
  reply(command, &status, 1);
}

// Путь из протокола приходит в стиле Wild Commander: '/' как разделитель,
// корень — сама выбранная папка. Выход за её пределы запрещён.
fs::path Z80Simulator::resolve(const std::string& path) const {
  std::string relative = path;
  while (!relative.empty() && relative.front() == '/') {
    relative.erase(relative.begin());
  }
  fs::path full = fs::path(root_);
  if (!relative.empty()) {
    full /= fs::path(relative);
  }
  return full.lexically_normal();
}

void Z80Simulator::handle(uint8_t command,
                          const std::vector<uint8_t>& payload) {
  switch (command) {
    case kVfsStat:
      handleStat(payload);
      break;
    case kVfsOpenDir:
      handleOpenDir(payload);
      break;
    case kVfsReadDir:
      handleReadDir();
      break;
    case kVfsFsInfo:
      handleFsInfo();
      break;
    case kVfsMkdir:
      handleMkdir(payload);
      break;
    case kVfsDelete:
      handleDelete(payload);
      break;
    case kVfsClose:
      handleClose();
      break;
    case kVfsOpen:
      handleOpen(payload);
      break;
    case kVfsSeek:
      handleSeek(payload);
      break;
    case kVfsReadWindow:
      handleReadWindow(payload);
      break;
    case kVfsWriteWindow:
      handleWriteWindow(payload);
      break;
    case kVfsSetEof:
      handleSetEof(payload);
      break;
    default:
      replyStatus(command, kStatusFail);
      break;
  }
}

void Z80Simulator::handleStat(const std::vector<uint8_t>& payload) {
  const std::string path(reinterpret_cast<const char*>(payload.data()),
                         strnlen(reinterpret_cast<const char*>(payload.data()),
                                 payload.size()));
  std::error_code code;
  const fs::path target = resolve(path);
  if (!fs::exists(target, code)) {
    replyStatus(kVfsStat, kStatusFail);
    return;
  }
  const bool directory = fs::is_directory(target, code);
  const uintmax_t size = directory ? 0 : fs::file_size(target, code);
  uint8_t answer[6] = {};
  answer[0] = kStatusOk;
  answer[1] = directory ? 1 : 0;
  writeLe32(answer + 2, static_cast<uint32_t>(size));
  reply(kVfsStat, answer, sizeof(answer));
}

// OPEN: [режим][путь,0]. Ответ — [статус][возможности][возможности FILEX].
// Ненулевой третий байт и открывает серверу позиционный режим OPEN=3; именно
// его отсутствие на железе давало отказ open-1.
void Z80Simulator::handleOpen(const std::vector<uint8_t>& payload) {
  if (payload.empty()) {
    replyStatus(kVfsOpen, kStatusFail);
    return;
  }
  const uint8_t mode = payload[0];
  const char* text = reinterpret_cast<const char*>(payload.data()) + 1;
  const std::string path(text, strnlen(text, payload.size() - 1));
  const fs::path target = resolve(path);
  std::error_code code;

  openValid_ = false;
  openMode_ = mode;
  offset_ = 0;
  windowActive_ = false;
  windowData_.clear();

  if (mode == 1) {
    // Замена: создаём пустой файл, старое содержимое отбрасывается.
    std::FILE* file = nullptr;
    fopen_s(&file, target.string().c_str(), "wb");
    if (file == nullptr) {
      replyStatus(kVfsOpen, kStatusFail);
      return;
    }
    std::fclose(file);
  } else if (!fs::exists(target, code)) {
    replyStatus(kVfsOpen, kStatusFail);
    return;
  }

  openPath_ = target;
  openValid_ = true;

  uint8_t answer[3];
  answer[0] = kStatusOk;
  answer[1] = 0x03;  // окна записи и чтения поддержаны
  answer[2] = mode == 3 ? 0x01 : 0x00;  // маска возможностей FILEX
  reply(kVfsOpen, answer, sizeof(answer));
}

void Z80Simulator::handleSeek(const std::vector<uint8_t>& payload) {
  if (openMode_ != 3) {
    replyStatus(kVfsSeek, kStatusFail);
    return;
  }
  if (!openValid_ || payload.size() < 4) {
    replyStatus(kVfsSeek, kStatusFail);
    return;
  }
  offset_ = readLe32(payload.data());
  replyStatus(kVfsSeek, kStatusOk);
}

// READ_WINDOW: запрос [seq][сколько:2]. Ответ — поток кадров одного окна:
// [status][flags][seq][offset:2][total:2][crc16:2][данные]. CRC считается по
// всему окну целиком и повторяется в каждом кадре.
void Z80Simulator::handleReadWindow(const std::vector<uint8_t>& payload) {
  if (!openValid_ || payload.size() < 3) {
    replyStatus(kVfsReadWindow, kStatusFail);
    return;
  }
  const uint8_t sequence = payload[0];
  size_t wanted = readLe16(payload.data() + 1);
  const size_t limit = openMode_ == 3 ? kFilexWindow : kSequentialWindow;
  if (wanted == 0 || wanted > limit) {
    replyStatus(kVfsReadWindow, kStatusFail);
    return;
  }
  // Последовательная ветка Z80 читает секторами по 512 байт: неполное окно
  // допустимо только как последнее в файле, иначе LOAD512 уйдёт вперёд
  // отправленного. Симулятор обязан отказывать там же, где откажет плагин.
  if (openMode_ != 3 && (wanted % 512) != 0) {
    std::error_code sizeCode;
    const uintmax_t total = fs::file_size(openPath_, sizeCode);
    const bool finalWindow =
        !sizeCode && static_cast<uintmax_t>(offset_) + wanted >= total;
    if (!finalWindow) {
      replyStatus(kVfsReadWindow, kStatusFail);
      return;
    }
  }

  std::FILE* file = nullptr;
  fopen_s(&file, openPath_.string().c_str(), "rb");
  if (file == nullptr) {
    replyStatus(kVfsReadWindow, kStatusFail);
    return;
  }
  std::fseek(file, static_cast<long>(offset_), SEEK_SET);
  std::vector<uint8_t> data(wanted);
  const size_t got = std::fread(data.data(), 1, wanted, file);
  std::fclose(file);
  if (got == 0) {
    // Конец файла: сервер отличает его по ненулевому статусу.
    replyStatus(kVfsReadWindow, kStatusFail);
    return;
  }
  data.resize(got);
  const uint16_t sum = crc16(data.data(), data.size());

  size_t sent = 0;
  const size_t chunk = kMaxPayload - kReadWindowHeader;
  while (sent < data.size()) {
    const size_t part = (std::min)(chunk, data.size() - sent);
    std::vector<uint8_t> frame(kReadWindowHeader + part);
    frame[0] = kStatusOk;
    frame[1] = static_cast<uint8_t>((sent == 0 ? kWindowStart : 0) |
                                    (sent + part == data.size() ? kWindowEnd : 0));
    frame[2] = sequence;
    writeLe16(frame.data() + 3, static_cast<uint16_t>(sent));
    writeLe16(frame.data() + 5, static_cast<uint16_t>(data.size()));
    writeLe16(frame.data() + 7, sum);
    std::memcpy(frame.data() + kReadWindowHeader, data.data() + sent, part);
    reply(kVfsReadWindow, frame.data(), frame.size());
    sent += part;
  }
  offset_ += static_cast<uint32_t>(data.size());
}

// WRITE_WINDOW: кадры [flags][seq][offset:2][total:2][crc16:2][данные].
// Подтверждение уходит одно на всё окно — [статус][seq][принято:2].
void Z80Simulator::handleWriteWindow(const std::vector<uint8_t>& payload) {
  if (!openValid_ || payload.size() < kWriteWindowHeader) {
    replyStatus(kVfsWriteWindow, kStatusFail);
    return;
  }
  const uint8_t flags = payload[0];
  const uint8_t sequence = payload[1];
  const uint16_t frameOffset = readLe16(payload.data() + 2);
  const uint16_t total = readLe16(payload.data() + 4);
  const uint16_t sum = readLe16(payload.data() + 6);
  const size_t part = payload.size() - kWriteWindowHeader;

  if ((flags & kWindowStart) != 0) {
    windowActive_ = true;
    windowSeq_ = sequence;
    windowTotal_ = total;
    windowData_.clear();
  }
  if (!windowActive_ || sequence != windowSeq_ ||
      frameOffset != windowData_.size()) {
    windowActive_ = false;
    uint8_t answer[4] = {kStatusFail, sequence, 0, 0};
    reply(kVfsWriteWindow, answer, sizeof(answer));
    return;
  }
  windowData_.insert(windowData_.end(),
                     payload.begin() + kWriteWindowHeader, payload.end());
  (void)part;

  if ((flags & kWindowEnd) == 0) {
    return;  // промежуточные кадры не подтверждаются
  }

  uint8_t answer[4] = {kStatusOk, sequence, 0, 0};
  const bool sane = windowData_.size() == windowTotal_ &&
                    crc16(windowData_.data(), windowData_.size()) == sum;
  if (sane) {
    std::FILE* file = nullptr;
    fopen_s(&file, openPath_.string().c_str(), "r+b");
    if (file == nullptr) {
      fopen_s(&file, openPath_.string().c_str(), "wb");
    }
    if (file != nullptr) {
      std::fseek(file, static_cast<long>(offset_), SEEK_SET);
      std::fwrite(windowData_.data(), 1, windowData_.size(), file);
      std::fclose(file);
      offset_ += static_cast<uint32_t>(windowData_.size());
      writeLe16(answer + 2, static_cast<uint16_t>(windowData_.size()));
    } else {
      answer[0] = kStatusFail;
    }
  } else {
    answer[0] = kStatusFail;
  }
  windowActive_ = false;
  windowData_.clear();
  reply(kVfsWriteWindow, answer, sizeof(answer));
}

// SET_EOF: [длина:4]. Ответ повторяет фактическую длину, по ней сервер
// проверяет, что усечение или расширение действительно применилось.
void Z80Simulator::handleSetEof(const std::vector<uint8_t>& payload) {
  if (!openValid_ || payload.size() < 4) {
    replyStatus(kVfsSetEof, kStatusFail);
    return;
  }
  const uint32_t size = readLe32(payload.data());
  std::error_code code;
  fs::resize_file(openPath_, size, code);
  if (code) {
    replyStatus(kVfsSetEof, kStatusFail);
    return;
  }
  uint8_t answer[5];
  answer[0] = kStatusOk;
  writeLe32(answer + 1, size);
  reply(kVfsSetEof, answer, sizeof(answer));
}

void Z80Simulator::handleClose() {
  openValid_ = false;
  windowActive_ = false;
  windowData_.clear();
  replyStatus(kVfsClose, kStatusOk);
}

void Z80Simulator::handleOpenDir(const std::vector<uint8_t>& payload) {
  const std::string path(reinterpret_cast<const char*>(payload.data()),
                         strnlen(reinterpret_cast<const char*>(payload.data()),
                                 payload.size()));
  std::error_code code;
  const fs::path target = resolve(path);
  entries_.clear();
  entryIndex_ = 0;
  if (!fs::is_directory(target, code)) {
    replyStatus(kVfsOpenDir, kStatusFail);
    return;
  }
  for (const auto& entry : fs::directory_iterator(target, code)) {
    Entry item;
    item.name = entry.path().filename().string();
    item.directory = entry.is_directory(code);
    item.size = item.directory
                    ? 0
                    : static_cast<uint32_t>(entry.file_size(code));
    entries_.push_back(std::move(item));
  }
  replyStatus(kVfsOpenDir, kStatusOk);
}

void Z80Simulator::handleReadDir() {
  if (entryIndex_ >= entries_.size()) {
    // Конец каталога сервер узнаёт по ненулевому статусу — так же, как на Z80.
    replyStatus(kVfsReadDir, kStatusFail);
    return;
  }
  const Entry& item = entries_[entryIndex_++];
  std::vector<uint8_t> answer(6 + item.name.size());
  answer[0] = kStatusOk;
  answer[1] = item.directory ? 1 : 0;
  writeLe32(answer.data() + 2, item.size);
  std::memcpy(answer.data() + 6, item.name.data(), item.name.size());
  reply(kVfsReadDir, answer.data(), answer.size());
}

void Z80Simulator::handleFsInfo() {
  std::error_code code;
  const fs::space_info space = fs::space(fs::path(root_), code);
  // Геометрия берётся правдоподобная для FAT32: сектор 512, кластер 32 КиБ.
  constexpr uint32_t kBytesPerSector = 512;
  constexpr uint8_t kSectorsPerCluster = 64;
  const uint64_t clusterBytes =
      static_cast<uint64_t>(kBytesPerSector) * kSectorsPerCluster;
  const uint32_t total =
      static_cast<uint32_t>(std::min<uint64_t>(space.capacity / clusterBytes,
                                               0x0FFFFFFFULL));
  const uint32_t free = static_cast<uint32_t>(
      std::min<uint64_t>(space.available / clusterBytes, total));

  uint8_t answer[24] = {};
  answer[0] = kStatusOk;
  answer[1] = 0x04 | 0x08;  // свободные кластеры достоверны, метка тома есть
  answer[2] = kSectorsPerCluster;
  answer[3] = static_cast<uint8_t>(kBytesPerSector);
  answer[4] = static_cast<uint8_t>(kBytesPerSector >> 8);
  writeLe32(answer + 5, total);
  writeLe32(answer + 9, free);
  writeLe32(answer + 13, 0x9016'4EF8u);
  std::memcpy(answer + 17, "HOSTSIM", 7);
  reply(kVfsFsInfo, answer, sizeof(answer));
}

void Z80Simulator::handleMkdir(const std::vector<uint8_t>& payload) {
  const std::string path(reinterpret_cast<const char*>(payload.data()),
                         strnlen(reinterpret_cast<const char*>(payload.data()),
                                 payload.size()));
  std::error_code code;
  const bool created = fs::create_directory(resolve(path), code);
  replyStatus(kVfsMkdir, created ? kStatusOk : kStatusFail);
}

void Z80Simulator::handleDelete(const std::vector<uint8_t>& payload) {
  const std::string path(reinterpret_cast<const char*>(payload.data()),
                         strnlen(reinterpret_cast<const char*>(payload.data()),
                                 payload.size()));
  std::error_code code;
  const bool removed = fs::remove(resolve(path), code);
  replyStatus(kVfsDelete, removed ? kStatusOk : kStatusFail);
}

}  // namespace host
}  // namespace zifi
