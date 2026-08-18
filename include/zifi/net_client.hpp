#pragma once

#include <WiFi.h>

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Один исходящий TCP-клиент. HTTP-заголовок разбирается на ESP, а тело остаётся
// в TCP и вытягивается Z80 командами NET_RECV.
// Все методы вызываются только сетевой задачей ядра 0.
class NetClient {
 public:
  NetClient();

  bool open(const char* host, uint16_t port, char* error, size_t errorSize);
  bool sendAll(const uint8_t* data, size_t length,
               char* error, size_t errorSize);
  bool receive(uint8_t* output, size_t limit, size_t& received, bool& eof,
               char* error, size_t errorSize);
  bool httpGet(const char* host, uint16_t port, const char* path,
               uint16_t& statusCode, uint32_t& contentLength,
               char* error, size_t errorSize);
  bool probe(const char* host, uint16_t port, uint16_t& elapsedMs);
  void setProxy(const char* host, uint16_t port);
  void clearProxy();
  bool proxyEnabled() const { return proxyEnabled_; }
  const char* proxyHost() const { return proxyHost_; }
  uint16_t proxyPort() const { return proxyPort_; }
  void close();
  bool active();

 private:
  bool readHttpHeader(uint16_t& statusCode, uint32_t& contentLength,
                      char* error, size_t errorSize);

  WiFiClient client_;
  char proxyHost_[128];
  uint16_t proxyPort_;
  bool proxyEnabled_;
  // Буфер заголовка живёт в объекте, а не на 6-КБ стеке сетевой задачи.
  char httpHeader_[2049];
  uint8_t pending_[256];
  size_t pendingOffset_;
  size_t pendingLength_;
};

}  // namespace zifi
