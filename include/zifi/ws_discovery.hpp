#pragma once

#include <stddef.h>
#include <stdint.h>

#include <WiFi.h>
#include <WiFiUdp.h>

#include "zifi/ws_discovery_xml.hpp"

namespace zifi {

// Современный Проводник Windows ищет компьютеры не через старый NetBIOS, а
// через WS-Discovery. Класс обслуживает только обнаружение и метаданные; SMB
// и доступ к SD остаются в SmbServer.
class WsDiscovery {
 public:
  WsDiscovery();
  ~WsDiscovery();

  bool begin(const char* hostname, const char* workgroup,
             const uint8_t deviceId[16], const char* firmwareVersion);
  void stop();
  void poll();
  bool running() const { return active_; }

 private:
  static constexpr uint16_t kUdpPort = 3702;
  static constexpr uint16_t kHttpPort = 5357;
  static constexpr size_t kUdpCapacity = 1460;
  static constexpr size_t kHttpRequestCapacity = 3072;
  static constexpr size_t kHttpResponseCapacity = 4096;
  static constexpr uint32_t kHttpTimeoutMs = 2500;
  // Повторное объявление нужно компьютерам, которые включились позже ESP или
  // очистили локальный кэш раздела «Сеть» после первого Hello.
  static constexpr uint32_t kHelloIntervalMs = 30000;

  WiFiUDP udp_;
  WiFiServer httpServer_;
  WiFiClient httpClient_;
  bool active_;
  uint32_t instanceId_;
  uint32_t messageNumber_;
  uint32_t lastHelloAt_;
  uint32_t httpStartedAt_;
  size_t httpRequestUsed_;

  char hostname_[32];
  char workgroup_[32];
  char firmwareVersion_[48];
  char uuid_[37];
  char sequenceUuid_[37];
  char ipAddress_[16];
  char udpBuffer_[kUdpCapacity];
  char httpRequest_[kHttpRequestCapacity];
  char httpResponse_[kHttpResponseCapacity];

  void pollUdp();
  void pollHttp();
  void acceptHttpClient();
  void handleHttpRequest(size_t headerLength, size_t bodyLength);
  void closeHttpClient();
  void sendHttpError(unsigned status, const char* reason);
  bool writeHttp(const char* data, size_t length);

  void sendHello();
  void sendBye();
  bool sendMulticast(const char* data, size_t length);
  bool sendUnicast(IPAddress address, uint16_t port, const char* data,
                   size_t length);

  void refreshIpAddress();
  void makeMessageId(char output[46]);
  wsd::Identity identity() const;
};

}  // namespace zifi
