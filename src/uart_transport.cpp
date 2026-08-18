#include "zifi/uart_transport.hpp"

#include <string.h>

namespace zifi {

UartTransport::UartTransport(HardwareSerial& serial)
    : serial_(serial),
      parser_(),
      rxChunk_{},
      rxChunkOffset_(0),
      rxChunkLength_(0) {}

void UartTransport::begin() {
  // RX-буфер больше максимального кадра: даже если Wi-Fi на короткое время
  // занял CPU, полный пакет Z80 не должен потеряться посередине.
  serial_.setRxBufferSize(2048);
  // Пины заданы явно: у ESP32-S3 псевдоним Serial может обозначать USB CDC,
  // тогда как переходник физически подключён к UART0 GPIO44/GPIO43.
  serial_.begin(kBaudRate, SERIAL_8N1, kRxPin, kTxPin, false);
  // Флаг ставится после begin(), когда драйвер UART уже создан.
  serial_.setDebugOutput(false);
  rxChunkOffset_ = 0;
  rxChunkLength_ = 0;
}

bool UartTransport::poll(PacketView& packet) {
  for (;;) {
    if (rxChunkOffset_ == rxChunkLength_) {
      rxChunkOffset_ = 0;
      rxChunkLength_ = 0;

      const int available = serial_.available();
      if (available <= 0) {
        parser_.checkTimeout(millis());
        return false;
      }
      const size_t wanted =
          static_cast<size_t>(available) < sizeof(rxChunk_)
              ? static_cast<size_t>(available)
              : sizeof(rxChunk_);
      rxChunkLength_ = serial_.read(rxChunk_, wanted);
      if (rxChunkLength_ == 0) {
        parser_.checkTimeout(millis());
        return false;
      }
    }

    // Для всех уже принятых байтов достаточно одного времени прихода. Раньше
    // millis() и HardwareSerial::read() вызывались для каждого байта кадра.
    const uint32_t receivedAt = millis();
    while (rxChunkOffset_ < rxChunkLength_) {
      const uint8_t value = rxChunk_[rxChunkOffset_++];
      if (!parser_.feed(value, receivedAt, packet)) {
        continue;
      }
      return true;
    }
  }
}

bool UartTransport::writerThunk(void* context, const uint8_t* data, size_t length) {
  return static_cast<UartTransport*>(context)->writeAll(data, length);
}

bool UartTransport::writeAll(const uint8_t* data, size_t length) {
  size_t offset = 0;
  uint32_t idleSince = millis();
  while (offset < length) {
    // UART/VFS работает на core 1, а Wi-Fi и SMB — на core 0. Поэтому кадр
    // можно передать одним блоком до 1024 байт без шестнадцати лишних yield().
    const size_t remaining = length - offset;
    const size_t part =
        remaining < kTxChunkSize ? remaining : kTxChunkSize;
    const size_t written = serial_.write(data + offset, part);
    if (written != 0) {
      offset += written;
      idleSince = millis();
      yield();
      continue;
    }
    if (static_cast<uint32_t>(millis() - idleSince) >= 2000) {
      return false;
    }
    delay(1);
  }
  return true;
}

bool UartTransport::send(uint8_t command, const uint8_t* data, uint16_t length) {
  return writeFrame(writerThunk, this, command, data, length);
}

bool UartTransport::sendError(const char* text) {
  if (text == nullptr) {
    text = "error";
  }
  const size_t textLength = strlen(text);
  const size_t sendLength = textLength < 48 ? textLength : 48;
  return send(kError, reinterpret_cast<const uint8_t*>(text),
              static_cast<uint16_t>(sendLength));
}

void UartTransport::flush() {
  serial_.flush();
}

bool UartTransport::waitFor(uint8_t wanted, uint32_t timeoutMs, PacketView& packet,
                            IdleHook idleHook, void* idleContext,
                            UnexpectedHandler unexpected,
                            void* unexpectedContext) {
  const uint32_t started = millis();
  // Вычитание беззнаковых millis() намеренно: оно корректно переживает
  // переполнение системного счётчика примерно раз в 49 суток.
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    if (idleHook != nullptr) {
      idleHook(idleContext);
    }
    if (poll(packet)) {
      if (packet.command == wanted) {
        return true;
      }
      if (unexpected != nullptr) {
        unexpected(unexpectedContext, packet);
      }
    } else {
      delay(1);
    }
    yield();
  }
  return false;
}

}  // namespace zifi
