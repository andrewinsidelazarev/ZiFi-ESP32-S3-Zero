#pragma once

#include <Arduino.h>

#include "zifi/protocol.hpp"

namespace zifi {

// Единственный владелец UART0 на ядре 1. Любая текстовая печать запрещена: Z80
// воспримет её как часть двоичного потока и потеряет синхронизацию.
class UartTransport {
 public:
  static constexpr int8_t kRxPin = 44;
  static constexpr int8_t kTxPin = 43;
  static constexpr uint32_t kBaudRate = 115200;

  using IdleHook = void (*)(void* context);
  using UnexpectedHandler = void (*)(void* context, const PacketView& packet);

  explicit UartTransport(HardwareSerial& serial);

  void begin();
  bool poll(PacketView& packet);
  bool send(uint8_t command, const uint8_t* data = nullptr, uint16_t length = 0);
  bool sendAck() { return send(kAck); }
  bool sendError(const char* text);
  void flush();

  // Ждёт ответ Z80 на VFS-команду. Необязательная фоновая функция оставлена
  // для служебных вызовов; в двухъядерном STOR сеть работает независимо.
  bool waitFor(uint8_t wanted, uint32_t timeoutMs, PacketView& packet,
               IdleHook idleHook = nullptr, void* idleContext = nullptr,
               UnexpectedHandler unexpected = nullptr,
               void* unexpectedContext = nullptr);

  const FrameParser& parser() const { return parser_; }

 private:
  static constexpr size_t kRxChunkSize = 256;
  static constexpr size_t kTxChunkSize = kMaxPayload;

  static bool writerThunk(void* context, const uint8_t* data, size_t length);
  bool writeAll(const uint8_t* data, size_t length);

  HardwareSerial& serial_;
  FrameParser parser_;
  // HardwareSerial умеет читать сразу массив. Непрочитанный хвост сохраняется
  // между poll(), поэтому два кадра из одного UART-блока не смешиваются и не
  // теряются после возврата первого PacketView.
  uint8_t rxChunk_[kRxChunkSize];
  size_t rxChunkOffset_;
  size_t rxChunkLength_;
};

}  // namespace zifi
