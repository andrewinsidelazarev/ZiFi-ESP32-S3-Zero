#include "zifi/protocol.hpp"

namespace zifi {

FrameParser::FrameParser()
    : state_(kWaitSync),
      command_(0),
      length_(0),
      position_(0),
      checksum_(0),
      lastByteMs_(0),
      badChecksumCount_(0),
      resyncCount_(0),
      payload_{} {}

void FrameParser::reset() {
  // Сам буфер тела очищать не нужно: длина следующего пакета определяет,
  // сколько байтов в нём действительно принадлежит пакету. Это экономит до
  // 1024 лишних записей при каждой команде.
  state_ = kWaitSync;
  length_ = 0;
  position_ = 0;
  checksum_ = 0;
}

bool FrameParser::checkTimeout(uint32_t nowMs) {
  // Если Z80 оборвал кадр посередине, бесконечно ждать продолжение нельзя:
  // следующий 0x5A должен снова восприниматься как начало нового кадра.
  if (state_ == kWaitSync || static_cast<uint32_t>(nowMs - lastByteMs_) < kFrameTimeoutMs) {
    return false;
  }
  ++resyncCount_;
  reset();
  return true;
}

bool FrameParser::feed(uint8_t byte, uint32_t nowMs, PacketView& packet) {
  lastByteMs_ = nowMs;
  switch (state_) {
    case kWaitSync:
      if (byte == kSync) {
        state_ = kCommand;
      }
      return false;

    case kCommand:
      command_ = byte;
      checksum_ = byte;
      state_ = kLengthLow;
      return false;

    case kLengthLow:
      length_ = byte;
      checksum_ ^= byte;
      state_ = kLengthHigh;
      return false;

    case kLengthHigh:
      length_ |= static_cast<uint16_t>(byte) << 8;
      checksum_ ^= byte;
      position_ = 0;
      if (length_ > kMaxPayload) {
        // Большая длина означает потерю синхронизации либо несовместимую
        // версию протокола. Не пытаемся пропустить указанное число байтов:
        // оно может быть случайным и задержит восстановление линии.
        ++resyncCount_;
        reset();
        return false;
      }
      state_ = length_ ? kData : kChecksum;
      return false;

    case kData:
      payload_[position_++] = byte;
      checksum_ ^= byte;
      if (position_ == length_) {
        state_ = kChecksum;
      }
      return false;

    case kChecksum:
      if (byte != checksum_) {
        ++badChecksumCount_;
        reset();
        return false;
      }
      packet.command = command_;
      // PacketView не копирует данные. Указатель действителен до следующего
      // принятого кадра; обработчик команды обязан использовать его сразу.
      packet.data = payload_;
      packet.length = length_;
      reset();
      return true;
  }
  reset();
  return false;
}

bool writeFrame(ByteWriter writer, void* context, uint8_t command,
                const uint8_t* data, uint16_t length) {
  if (writer == nullptr || length > kMaxPayload || (length != 0 && data == nullptr)) {
    return false;
  }
  uint8_t header[4] = {
      kSync,
      command,
      static_cast<uint8_t>(length),
      static_cast<uint8_t>(length >> 8),
  };
  uint8_t checksum = header[1] ^ header[2] ^ header[3];
  // SYNC 0x5A специально не входит в XOR — это контракт существующего
  // zifi.spg, менять его в нативной прошивке нельзя.
  for (uint16_t i = 0; i < length; ++i) {
    checksum ^= data[i];
  }
  return writer(context, header, sizeof(header)) &&
         (length == 0 || writer(context, data, length)) &&
         writer(context, &checksum, 1);
}

uint16_t crc16CcittFalseUpdate(uint16_t crc, const uint8_t* data,
                               size_t length) {
  // Потоковая форма нужна окну 16 КиБ: CRC считается по мере копирования
  // фрагментов из PSRAM, без второго прохода и отдельного большого буфера.
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

uint16_t crc16CcittFalse(const uint8_t* data, size_t length) {
  return crc16CcittFalseUpdate(0xFFFF, data, length);
}

uint32_t crc32IsoHdlc(const uint8_t* data, size_t length) {
  // Контрольная сумма полного zifi.ini. Она вынесена в чистое C++-ядро,
  // поэтому тот же код проверяется host-тестом без Arduino и сети.
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL &
                          static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1))));
    }
  }
  return ~crc;
}

}  // namespace zifi
