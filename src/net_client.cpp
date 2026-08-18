#include "zifi/net_client.hpp"

#include <Arduino.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace zifi {
namespace {

void setError(char* error, size_t size, const char* text) {
  if (error != nullptr && size != 0) {
    snprintf(error, size, "%s", text == nullptr ? "error" : text);
  }
}

}  // namespace

NetClient::NetClient()
    : client_(),
      proxyHost_{},
      proxyPort_(0),
      proxyEnabled_(false),
      httpHeader_{},
      pending_{},
      pendingOffset_(0),
      pendingLength_(0) {}

bool NetClient::active() {
  return client_.connected() || client_.available() > 0 ||
         pendingLength_ > pendingOffset_;
}

void NetClient::close() {
  client_.stop();
  pendingOffset_ = 0;
  pendingLength_ = 0;
}

bool NetClient::open(const char* host, uint16_t port,
                     char* error, size_t errorSize) {
  close();
  if (host == nullptr || *host == 0) {
    setError(error, errorSize, "no host");
    return false;
  }
  client_.setNoDelay(true);
  // У ESP32 перегрузка connect принимает миллисекунды, а setTimeout — секунды.
  // Задаём их раздельно, чтобы недоступный узел не подвесил Z80 на часы.
  if (!client_.connect(host, port, 8000)) {
    close();
    setError(error, errorSize, "connect failed");
    return false;
  }
  client_.setTimeout(8);
  setError(error, errorSize, "");
  return true;
}

bool NetClient::sendAll(const uint8_t* data, size_t length,
                        char* error, size_t errorSize) {
  if (!active()) {
    setError(error, errorSize, "not open");
    return false;
  }
  if (data == nullptr && length != 0) {
    setError(error, errorSize, "send null");
    return false;
  }
  size_t offset = 0;
  uint32_t idleSince = millis();
  while (offset < length) {
    const size_t written = client_.write(data + offset, length - offset);
    if (written != 0) {
      offset += written;
      idleSince = millis();
      vTaskDelay(1);
      continue;
    }
    if (!client_.connected() ||
        static_cast<uint32_t>(millis() - idleSince) >= 8000) {
      setError(error, errorSize, "short send");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  setError(error, errorSize, "");
  return true;
}

bool NetClient::receive(uint8_t* output, size_t limit, size_t& received,
                        bool& eof, char* error, size_t errorSize) {
  received = 0;
  eof = false;
  if (output == nullptr || limit == 0) {
    setError(error, errorSize, "receive range");
    return false;
  }

  if (pendingOffset_ < pendingLength_) {
    const size_t pending = pendingLength_ - pendingOffset_;
    received = limit < pending ? limit : pending;
    memcpy(output, pending_ + pendingOffset_, received);
    pendingOffset_ += received;
    if (pendingOffset_ == pendingLength_) {
      pendingOffset_ = pendingLength_ = 0;
    }
    setError(error, errorSize, "");
    return true;
  }

  const int available = client_.available();
  if (available > 0) {
    const size_t have = static_cast<size_t>(available);
    const size_t wanted = limit < have ? limit : have;
    const int count = client_.read(output, wanted);
    if (count < 0) {
      setError(error, errorSize, "recv failed");
      return false;
    }
    received = static_cast<size_t>(count);
    setError(error, errorSize, "");
    return true;
  }

  // Пустой живой сокет — пауза, закрытый пустой сокет — настоящий EOF.
  eof = !client_.connected();
  if (eof) {
    close();
  }
  setError(error, errorSize, "");
  return true;
}

bool NetClient::readHttpHeader(uint16_t& statusCode, uint32_t& contentLength,
                               char* error, size_t errorSize) {
  char* header = httpHeader_;
  size_t used = 0;
  const uint32_t started = millis();
  int bodyAt = -1;

  while (static_cast<uint32_t>(millis() - started) < 10000) {
    while (client_.available() > 0 && used < sizeof(httpHeader_) - 1) {
      const int value = client_.read();
      if (value < 0) {
        break;
      }
      header[used++] = static_cast<char>(value);
      if (used >= 4 && memcmp(header + used - 4, "\r\n\r\n", 4) == 0) {
        bodyAt = static_cast<int>(used);
        break;
      }
    }
    if (bodyAt >= 0) {
      break;
    }
    if (!client_.connected() && client_.available() == 0) {
      setError(error, errorSize, "closed in header");
      return false;
    }
    if (used == sizeof(httpHeader_) - 1) {
      setError(error, errorSize, "header too long");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (bodyAt < 0) {
    setError(error, errorSize, "header timeout");
    return false;
  }

  header[used] = 0;
  statusCode = 0;
  contentLength = 0;
  const char* firstSpace = strchr(header, ' ');
  if (firstSpace != nullptr) {
    statusCode = static_cast<uint16_t>(strtoul(firstSpace + 1, nullptr, 10));
  }
  const char* line = header;
  while (line < header + bodyAt) {
    const char* next = strstr(line, "\r\n");
    if (next == nullptr) {
      break;
    }
    constexpr char kName[] = "content-length:";
    bool match = static_cast<size_t>(next - line) >= sizeof(kName) - 1;
    for (size_t index = 0; match && index < sizeof(kName) - 1; ++index) {
      match = static_cast<char>(
                  tolower(static_cast<unsigned char>(line[index]))) ==
              kName[index];
    }
    if (match) {
      contentLength = strtoul(line + sizeof(kName) - 1, nullptr, 10);
      break;
    }
    line = next + 2;
  }

  pendingOffset_ = 0;
  pendingLength_ = 0;
  setError(error, errorSize, "");
  return true;
}

void NetClient::setProxy(const char* host, uint16_t port) {
  if (host != nullptr && *host != 0) {
    snprintf(proxyHost_, sizeof(proxyHost_), "%s", host);
    proxyPort_ = port != 0 ? port : 49281;
    proxyEnabled_ = true;
  } else {
    clearProxy();
  }
}

void NetClient::clearProxy() {
  proxyHost_[0] = 0;
  proxyPort_ = 0;
  proxyEnabled_ = false;
}

bool NetClient::httpGet(const char* host, uint16_t port, const char* path,
                        uint16_t& statusCode, uint32_t& contentLength,
                        char* error, size_t errorSize) {
  const bool useProxy = proxyEnabled_ && proxyHost_[0] != 0;
  const char* connectHost = useProxy ? proxyHost_ : host;
  const uint16_t connectPort = useProxy ? proxyPort_ : port;

  if (!open(connectHost, connectPort, error, errorSize)) {
    return false;
  }
  if (path == nullptr || *path == 0) {
    path = "/";
  }
  char request[640];
  int length = -1;
  if (useProxy) {
    if (port != 80 && port != 0) {
      length = snprintf(
          request, sizeof(request),
          "GET http://%s:%u%s HTTP/1.0\r\n"
          "Host: %s:%u\r\n"
          "Proxy-Authorization: Basic eng6eng=\r\n"
          "User-Agent: ZiFi (ZX Evo)\r\n"
          "Accept: */*\r\n"
          "Connection: close\r\n\r\n",
          host, port, path, host, port);
    } else {
      length = snprintf(
          request, sizeof(request),
          "GET http://%s%s HTTP/1.0\r\n"
          "Host: %s\r\n"
          "Proxy-Authorization: Basic eng6eng=\r\n"
          "User-Agent: ZiFi (ZX Evo)\r\n"
          "Accept: */*\r\n"
          "Connection: close\r\n\r\n",
          host, path, host);
    }
  } else {
    if (port != 80 && port != 0) {
      length = snprintf(
          request, sizeof(request),
          "GET %s HTTP/1.0\r\n"
          "Host: %s:%u\r\n"
          "User-Agent: ZiFi (ZX Evo)\r\n"
          "Accept: */*\r\n"
          "Connection: close\r\n\r\n",
          path, host, port);
    } else {
      length = snprintf(
          request, sizeof(request),
          "GET %s HTTP/1.0\r\n"
          "Host: %s\r\n"
          "User-Agent: ZiFi (ZX Evo)\r\n"
          "Accept: */*\r\n"
          "Connection: close\r\n\r\n",
          path, host);
    }
  }
  if (length < 0 || static_cast<size_t>(length) >= sizeof(request)) {
    close();
    setError(error, errorSize, "request too long");
    return false;
  }
  if (!sendAll(reinterpret_cast<const uint8_t*>(request),
               static_cast<size_t>(length), error, errorSize) ||
      !readHttpHeader(statusCode, contentLength, error, errorSize)) {
    close();
    return false;
  }
  return true;
}

bool NetClient::probe(const char* host, uint16_t port, uint16_t& elapsedMs) {
  WiFiClient probeClient;
  const uint32_t started = millis();
  const bool connected = probeClient.connect(host, port, 3000);
  const uint32_t elapsed = millis() - started;
  probeClient.stop();
  elapsedMs = static_cast<uint16_t>(elapsed < 0xFFFFUL ? elapsed : 0xFFFFUL);
  return connected;
}

}  // namespace zifi
