#pragma once

#include <WiFi.h>

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Транзакционное обновление цельного firmware.bin по TCP. Образ пишется в
// неактивный OTA-раздел; загрузочным он становится только после полного
// приёма, проверки типа ESP32-S3 и SHA-256.
class OtaServer {
 public:
  static constexpr uint16_t kDefaultPort = 8267;

  OtaServer();

  bool start(uint16_t port, char* error, size_t errorSize);
  void stop();
  void poll();
  bool running() const { return running_; }
  uint16_t port() const { return port_; }

 private:
  static constexpr size_t kLineSize = 128;
  static constexpr size_t kChunkSize = 1024;

  void acceptClient();
  void receiveBeginLine();
  void processBegin();
  bool sendDiagnosticLog();
  bool receiveFirmware(uint32_t size, const uint8_t expectedSha[32]);
  void closeClient();
  bool sendBuffer(const uint8_t* data, size_t length);
  bool sendText(const char* text);
  bool sendFormat(const char* format, ...);
  static bool parseSha256(const char* text, uint8_t output[32]);

  WiFiServer server_;
  WiFiClient client_;
  bool running_;
  bool clientActive_;
  uint16_t port_;
  uint32_t acceptedAtMs_;
  char line_[kLineSize + 1];
  size_t lineLength_;
  uint8_t chunk_[kChunkSize];
};

}  // namespace zifi
