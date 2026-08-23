#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>

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
  bool openTls(const char* host, uint16_t port,
               char* error, size_t errorSize);
  bool applyRedirect(uint16_t& port, bool& useTls,
                     char* error, size_t errorSize);
  bool readHttpHeader(uint16_t& statusCode, uint32_t& contentLength,
                      char* error, size_t errorSize);
  bool tlsPeerClosed() const;

  WiFiClient client_;
  WiFiClientSecure secureClient_;
  WiFiClient* transport_;
  bool transportTls_;
  bool caBundleLoaded_;
  bool bodyActive_;
  bool bodyLengthKnown_;
  uint32_t bodyExpected_;
  uint32_t bodyReceived_;
  uint32_t bodyLastActivityMs_;
  char proxyHost_[128];
  uint16_t proxyPort_;
  bool proxyEnabled_;
  // Крупные HTTP-буферы живут в объекте, а не на стеке сетевой задачи: поверх
  // httpGet на том же стеке выполняется ресурсоёмкое TLS-рукопожатие mbedTLS.
  char httpHeader_[2049];
  char httpHost_[128];
  char httpPath_[384];
  char httpRequest_[640];
  char redirectLocation_[512];
  uint8_t pending_[256];
  size_t pendingOffset_;
  size_t pendingLength_;
};

}  // namespace zifi
