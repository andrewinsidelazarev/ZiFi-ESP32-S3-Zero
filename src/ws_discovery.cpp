#include "zifi/ws_discovery.hpp"

#include <Arduino.h>

#include <ctype.h>
#include <esp_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace zifi {
namespace {

const IPAddress kWsdMulticast(239, 255, 255, 250);

size_t minimum(size_t left, size_t right) {
  return left < right ? left : right;
}

void formatUuid(const uint8_t bytes[16], char output[37]) {
  snprintf(output, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
           bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
           bytes[12], bytes[13], bytes[14], bytes[15]);
}

void makeUuidFromBytes(const uint8_t input[16], char output[37]) {
  uint8_t bytes[16] = {};
  memcpy(bytes, input, sizeof(bytes));
  // Эти биты делают значение обычным UUID версии 5 с RFC-вариантом. Само
  // значение остаётся устойчивым, потому что основано на заводском MAC ESP.
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x50);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
  formatUuid(bytes, output);
}

void makeRandomUuid(char output[37]) {
  uint8_t bytes[16] = {};
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
  formatUuid(bytes, output);
}

bool sameNoCase(const char* left, size_t length, const char* right) {
  if (left == nullptr || right == nullptr || strlen(right) != length) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (tolower(static_cast<unsigned char>(left[index])) !=
        tolower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

const char* findHeaderEnd(const char* request, size_t length) {
  if (request == nullptr || length < 4) {
    return nullptr;
  }
  for (size_t index = 0; index + 3 < length; ++index) {
    if (request[index] == '\r' && request[index + 1] == '\n' &&
        request[index + 2] == '\r' && request[index + 3] == '\n') {
      return request + index + 4;
    }
  }
  return nullptr;
}

bool parseContentLength(const char* request, size_t headerLength,
                        size_t& contentLength) {
  contentLength = 0;
  const char* cursor = request;
  const char* const end = request + headerLength;
  while (cursor < end) {
    const char* lineEnd = cursor;
    while (lineEnd + 1 < end &&
           !(lineEnd[0] == '\r' && lineEnd[1] == '\n')) {
      ++lineEnd;
    }
    const char* colon = cursor;
    while (colon < lineEnd && *colon != ':') {
      ++colon;
    }
    if (colon < lineEnd &&
        sameNoCase(cursor, static_cast<size_t>(colon - cursor),
                   "Content-Length")) {
      const char* value = colon + 1;
      while (value < lineEnd &&
             isspace(static_cast<unsigned char>(*value)) != 0) {
        ++value;
      }
      if (value == lineEnd) {
        return false;
      }
      char* parsedEnd = nullptr;
      const unsigned long parsed = strtoul(value, &parsedEnd, 10);
      if (parsedEnd == value || parsedEnd > lineEnd) {
        return false;
      }
      while (parsedEnd < lineEnd &&
             isspace(static_cast<unsigned char>(*parsedEnd)) != 0) {
        ++parsedEnd;
      }
      if (parsedEnd != lineEnd) {
        return false;
      }
      contentLength = static_cast<size_t>(parsed);
      return true;
    }
    if (lineEnd + 2 > end) {
      break;
    }
    cursor = lineEnd + 2;
  }
  return false;
}

bool requestPathMatches(const char* request, size_t headerLength,
                        const char* uuid) {
  if (request == nullptr || uuid == nullptr || headerLength < 7 ||
      memcmp(request, "POST ", 5) != 0) {
    return false;
  }
  const char* path = request + 5;
  const char* const end = request + headerLength;
  const char* pathEnd = path;
  while (pathEnd < end && *pathEnd != ' ' && *pathEnd != '\r' &&
         *pathEnd != '\n') {
    ++pathEnd;
  }
  char expected[40] = {};
  const int written = snprintf(expected, sizeof(expected), "/%s", uuid);
  return written > 0 && static_cast<size_t>(written) < sizeof(expected) &&
         static_cast<size_t>(pathEnd - path) == static_cast<size_t>(written) &&
         memcmp(path, expected, static_cast<size_t>(written)) == 0;
}

}  // namespace

WsDiscovery::WsDiscovery()
    : udp_(),
      httpServer_(kHttpPort, 1),
      httpClient_(),
      active_(false),
      instanceId_(0),
      messageNumber_(1),
      lastHelloAt_(0),
      httpStartedAt_(0),
      httpRequestUsed_(0),
      hostname_{},
      workgroup_{},
      firmwareVersion_{},
      uuid_{},
      sequenceUuid_{},
      ipAddress_{},
      udpBuffer_{},
      httpRequest_{},
      httpResponse_{} {}

WsDiscovery::~WsDiscovery() {
  stop();
}

bool WsDiscovery::begin(const char* hostname, const char* workgroup,
                        const uint8_t deviceId[16],
                        const char* firmwareVersion) {
  stop();
  if (hostname == nullptr || *hostname == 0 || workgroup == nullptr ||
      *workgroup == 0 || deviceId == nullptr || firmwareVersion == nullptr ||
      strlen(hostname) >= sizeof(hostname_) ||
      strlen(workgroup) >= sizeof(workgroup_) ||
      strlen(firmwareVersion) >= sizeof(firmwareVersion_)) {
    return false;
  }

  snprintf(hostname_, sizeof(hostname_), "%s", hostname);
  snprintf(workgroup_, sizeof(workgroup_), "%s", workgroup);
  snprintf(firmwareVersion_, sizeof(firmwareVersion_), "%s", firmwareVersion);
  makeUuidFromBytes(deviceId, uuid_);
  makeRandomUuid(sequenceUuid_);
  refreshIpAddress();
  instanceId_ = esp_random() & 0x7FFFFFFFUL;
  if (instanceId_ == 0) {
    instanceId_ = 1;
  }
  messageNumber_ = 1;

  // UDP/3702 принимает multicast Probe, TCP/5357 отдаёт описание компьютера.
  // Если любой сокет не открылся, не объявляем половинчатую WSD-службу.
  if (udp_.beginMulticast(kWsdMulticast, kUdpPort) != 1) {
    return false;
  }
  httpServer_.begin(kHttpPort, 1);
  if (!httpServer_) {
    udp_.stop();
    return false;
  }
  active_ = true;
  sendHello();
  return true;
}

void WsDiscovery::stop() {
  if (active_) {
    sendBye();
  }
  closeHttpClient();
  httpServer_.end();
  udp_.stop();
  active_ = false;
  lastHelloAt_ = 0;
  httpRequestUsed_ = 0;
}

void WsDiscovery::poll() {
  if (!active_) {
    return;
  }
  if (static_cast<uint32_t>(millis() - lastHelloAt_) >=
      kHelloIntervalMs) {
    sendHello();
  }
  pollUdp();
  pollHttp();
}

void WsDiscovery::pollUdp() {
  const int packetLength = udp_.parsePacket();
  if (packetLength <= 0) {
    return;
  }
  const IPAddress remoteAddress = udp_.remoteIP();
  const uint16_t remotePort = udp_.remotePort();
  const size_t wanted = minimum(static_cast<size_t>(packetLength),
                                sizeof(udpBuffer_) - 1);
  const int received = udp_.read(udpBuffer_, wanted);
  while (udp_.available() > 0) {
    udp_.read();
  }
  if (received <= 0 || static_cast<size_t>(packetLength) >= sizeof(udpBuffer_)) {
    return;
  }
  udpBuffer_[received] = 0;

  const wsd::RequestKind kind =
      wsd::classifyRequest(udpBuffer_, static_cast<size_t>(received));
  if (kind != wsd::RequestKind::kProbe &&
      kind != wsd::RequestKind::kResolve) {
    return;
  }
  if (kind == wsd::RequestKind::kResolve &&
      !wsd::containsEndpoint(udpBuffer_, static_cast<size_t>(received), uuid_)) {
    return;
  }

  char requestId[96] = {};
  if (!wsd::extractElementText(udpBuffer_, static_cast<size_t>(received),
                               "MessageID", requestId, sizeof(requestId))) {
    return;
  }
  refreshIpAddress();
  char responseId[46] = {};
  makeMessageId(responseId);
  const wsd::Identity current = identity();
  const uint32_t number = messageNumber_++;
  const size_t responseLength =
      kind == wsd::RequestKind::kProbe
          ? wsd::buildProbeMatches(udpBuffer_, sizeof(udpBuffer_), current,
                                   responseId, requestId, instanceId_, number)
          : wsd::buildResolveMatches(udpBuffer_, sizeof(udpBuffer_), current,
                                     responseId, requestId, instanceId_, number);
  if (responseLength != 0) {
    sendUnicast(remoteAddress, remotePort, udpBuffer_, responseLength);
  }
}

void WsDiscovery::acceptHttpClient() {
  if (httpClient_) {
    return;
  }
  WiFiClient incoming = httpServer_.available();
  if (!incoming) {
    return;
  }
  httpClient_ = incoming;
  httpClient_.setNoDelay(true);
  httpClient_.setTimeout(2);
  httpRequestUsed_ = 0;
  httpStartedAt_ = millis();
  httpRequest_[0] = 0;
}

void WsDiscovery::pollHttp() {
  if (!httpClient_) {
    acceptHttpClient();
  }
  if (!httpClient_) {
    return;
  }

  while (httpClient_.available() > 0) {
    if (httpRequestUsed_ + 1 >= sizeof(httpRequest_)) {
      sendHttpError(413, "Payload Too Large");
      return;
    }
    const size_t room = sizeof(httpRequest_) - 1 - httpRequestUsed_;
    const size_t wanted = minimum(static_cast<size_t>(httpClient_.available()),
                                  room);
    const int received = httpClient_.read(
        reinterpret_cast<uint8_t*>(httpRequest_ + httpRequestUsed_), wanted);
    if (received <= 0) {
      break;
    }
    httpRequestUsed_ += static_cast<size_t>(received);
    httpRequest_[httpRequestUsed_] = 0;
  }

  const char* body = findHeaderEnd(httpRequest_, httpRequestUsed_);
  if (body != nullptr) {
    const size_t headerLength = static_cast<size_t>(body - httpRequest_);
    size_t bodyLength = 0;
    if (!parseContentLength(httpRequest_, headerLength, bodyLength)) {
      sendHttpError(411, "Length Required");
      return;
    }
    if (headerLength + bodyLength >= sizeof(httpRequest_)) {
      sendHttpError(413, "Payload Too Large");
      return;
    }
    if (httpRequestUsed_ >= headerLength + bodyLength) {
      handleHttpRequest(headerLength, bodyLength);
      return;
    }
  }

  if ((!httpClient_.connected() && httpClient_.available() == 0) ||
      static_cast<uint32_t>(millis() - httpStartedAt_) > kHttpTimeoutMs) {
    closeHttpClient();
  }
}

void WsDiscovery::handleHttpRequest(size_t headerLength, size_t bodyLength) {
  const char* body = httpRequest_ + headerLength;
  if (!requestPathMatches(httpRequest_, headerLength, uuid_) ||
      wsd::classifyRequest(body, bodyLength) != wsd::RequestKind::kGet) {
    sendHttpError(400, "Bad Request");
    return;
  }
  char requestId[96] = {};
  if (!wsd::extractElementText(body, bodyLength, "MessageID", requestId,
                               sizeof(requestId))) {
    sendHttpError(400, "Bad Request");
    return;
  }

  refreshIpAddress();
  char responseId[46] = {};
  makeMessageId(responseId);
  const size_t responseLength = wsd::buildGetResponse(
      httpResponse_, sizeof(httpResponse_), identity(), responseId, requestId);
  if (responseLength == 0) {
    sendHttpError(500, "Internal Server Error");
    return;
  }

  char header[192] = {};
  const int headerLengthWritten = snprintf(
      header, sizeof(header),
      "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml; charset=utf-8\r\n"
      "Content-Length: %u\r\nConnection: close\r\n\r\n",
      static_cast<unsigned>(responseLength));
  if (headerLengthWritten > 0 &&
      static_cast<size_t>(headerLengthWritten) < sizeof(header)) {
    writeHttp(header, static_cast<size_t>(headerLengthWritten));
    writeHttp(httpResponse_, responseLength);
  }
  closeHttpClient();
}

void WsDiscovery::closeHttpClient() {
  httpClient_.stop();
  httpClient_ = WiFiClient();
  httpRequestUsed_ = 0;
}

void WsDiscovery::sendHttpError(unsigned status, const char* reason) {
  char response[160] = {};
  const int written = snprintf(
      response, sizeof(response),
      "HTTP/1.1 %u %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
      status, reason == nullptr ? "Error" : reason);
  if (written > 0 && static_cast<size_t>(written) < sizeof(response)) {
    writeHttp(response, static_cast<size_t>(written));
  }
  closeHttpClient();
}

bool WsDiscovery::writeHttp(const char* data, size_t length) {
  size_t sent = 0;
  const uint32_t started = millis();
  while (sent < length && httpClient_.connected() &&
         static_cast<uint32_t>(millis() - started) < kHttpTimeoutMs) {
    const size_t count = httpClient_.write(
        reinterpret_cast<const uint8_t*>(data + sent), length - sent);
    if (count == 0) {
      delay(1);
    } else {
      sent += count;
    }
  }
  return sent == length;
}

void WsDiscovery::sendHello() {
  // Отмечаем попытку до отправки: при временной сетевой ошибке основной цикл
  // не должен превращаться в непрерывный multicast-шторм.
  lastHelloAt_ = millis();
  refreshIpAddress();
  char messageId[46] = {};
  makeMessageId(messageId);
  const size_t length = wsd::buildHello(
      udpBuffer_, sizeof(udpBuffer_), identity(), messageId, instanceId_,
      messageNumber_++);
  if (length != 0) {
    sendMulticast(udpBuffer_, length);
  }
}

void WsDiscovery::sendBye() {
  char messageId[46] = {};
  makeMessageId(messageId);
  const size_t length = wsd::buildBye(
      udpBuffer_, sizeof(udpBuffer_), identity(), messageId, instanceId_,
      messageNumber_++);
  if (length != 0) {
    sendMulticast(udpBuffer_, length);
  }
}

bool WsDiscovery::sendMulticast(const char* data, size_t length) {
  if (!active_ || data == nullptr || length == 0 ||
      length >= kUdpCapacity || udp_.beginMulticastPacket() != 1) {
    return false;
  }
  return udp_.write(reinterpret_cast<const uint8_t*>(data), length) == length &&
         udp_.endPacket() == 1;
}

bool WsDiscovery::sendUnicast(IPAddress address, uint16_t port,
                              const char* data, size_t length) {
  if (!active_ || data == nullptr || length == 0 ||
      length >= kUdpCapacity || udp_.beginPacket(address, port) != 1) {
    return false;
  }
  return udp_.write(reinterpret_cast<const uint8_t*>(data), length) == length &&
         udp_.endPacket() == 1;
}

void WsDiscovery::refreshIpAddress() {
  const IPAddress address = WiFi.localIP();
  snprintf(ipAddress_, sizeof(ipAddress_), "%u.%u.%u.%u", address[0],
           address[1], address[2], address[3]);
}

void WsDiscovery::makeMessageId(char output[46]) {
  char uuid[37] = {};
  makeRandomUuid(uuid);
  snprintf(output, 46, "urn:uuid:%s", uuid);
}

wsd::Identity WsDiscovery::identity() const {
  return {uuid_, sequenceUuid_, hostname_, workgroup_, firmwareVersion_,
          ipAddress_};
}

}  // namespace zifi
