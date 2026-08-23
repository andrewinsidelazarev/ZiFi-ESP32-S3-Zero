#include "zifi/net_client.hpp"

#include <Arduino.h>

#include <ctype.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace zifi {
namespace {

extern const uint8_t kCaBundleStart[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

void setError(char* error, size_t size, const char* text) {
  if (error != nullptr && size != 0) {
    snprintf(error, size, "%s", text == nullptr ? "error" : text);
  }
}

bool startsWithIgnoreCase(const char* begin, const char* end,
                          const char* prefix) {
  while (*prefix != 0) {
    if (begin == end ||
        tolower(static_cast<unsigned char>(*begin)) !=
            tolower(static_cast<unsigned char>(*prefix))) {
      return false;
    }
    ++begin;
    ++prefix;
  }
  return true;
}

bool containsIgnoreCase(const char* begin, const char* end,
                        const char* token) {
  const size_t tokenLength = strlen(token);
  for (const char* at = begin; at + tokenLength <= end; ++at) {
    if (startsWithIgnoreCase(at, end, token)) {
      return true;
    }
  }
  return false;
}

bool isRedirectStatus(uint16_t statusCode) {
  return statusCode == 301 || statusCode == 302 || statusCode == 303 ||
         statusCode == 307 || statusCode == 308;
}

}  // namespace

NetClient::NetClient()
    : client_(),
      secureClient_(),
      transport_(nullptr),
      transportTls_(false),
      caBundleLoaded_(false),
      bodyActive_(false),
      bodyLengthKnown_(false),
      bodyExpected_(0),
      bodyReceived_(0),
      bodyLastActivityMs_(0),
      proxyHost_{},
      proxyPort_(0),
      proxyEnabled_(false),
      httpHeader_{},
      httpHost_{},
      httpPath_{},
      httpRequest_{},
      redirectLocation_{},
      pending_{},
      pendingOffset_(0),
      pendingLength_(0) {}

bool NetClient::active() {
  return pendingLength_ > pendingOffset_ ||
         (transport_ != nullptr &&
          (transport_->connected() || transport_->available() > 0));
}

void NetClient::close() {
  client_.stop();
  secureClient_.stop();
  transport_ = nullptr;
  transportTls_ = false;
  bodyActive_ = false;
  bodyLengthKnown_ = false;
  bodyExpected_ = 0;
  bodyReceived_ = 0;
  bodyLastActivityMs_ = 0;
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
  transport_ = &client_;
  transportTls_ = false;
  setError(error, errorSize, "");
  return true;
}

bool NetClient::openTls(const char* host, uint16_t port,
                        char* error, size_t errorSize) {
  close();
  if (host == nullptr || *host == 0) {
    setError(error, errorSize, "no host");
    return false;
  }
  if (!caBundleLoaded_) {
    secureClient_.setCACertBundle(kCaBundleStart);
    caBundleLoaded_ = true;
  }
  secureClient_.setNoDelay(true);
  secureClient_.setHandshakeTimeout(12);
  if (!secureClient_.connect(host, port, 12000)) {
    char detail[48]{};
    secureClient_.lastError(detail, sizeof(detail));
    close();
    if (detail[0] != 0) {
      if (error != nullptr && errorSize != 0) {
        snprintf(error, errorSize, "tls: %.40s", detail);
      }
    } else {
      setError(error, errorSize, "tls connect failed");
    }
    return false;
  }
  secureClient_.setTimeout(8);
  transport_ = &secureClient_;
  transportTls_ = true;
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
    const size_t written = transport_->write(data + offset, length - offset);
    if (written != 0) {
      offset += written;
      idleSince = millis();
      vTaskDelay(1);
      continue;
    }
    if (!transport_->connected() ||
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

  // Content-Length является границей тела HTTP, поэтому ждать отдельного FIN
  // от TLS после последнего байта не требуется. WiFiClientSecure некоторых
  // версий оставляет connected() истинным после close_notify.
  if (bodyActive_ && bodyLengthKnown_ && bodyReceived_ >= bodyExpected_) {
    eof = true;
    close();
    setError(error, errorSize, "");
    return true;
  }

  if (pendingOffset_ < pendingLength_) {
    const size_t pending = pendingLength_ - pendingOffset_;
    received = limit < pending ? limit : pending;
    if (bodyActive_ && bodyLengthKnown_) {
      const uint32_t remaining = bodyExpected_ - bodyReceived_;
      if (received > remaining) {
        received = static_cast<size_t>(remaining);
      }
    }
    memcpy(output, pending_ + pendingOffset_, received);
    pendingOffset_ += received;
    if (pendingOffset_ == pendingLength_) {
      pendingOffset_ = pendingLength_ = 0;
    }
    if (bodyActive_) {
      bodyReceived_ += static_cast<uint32_t>(received);
      bodyLastActivityMs_ = millis();
    }
    setError(error, errorSize, "");
    return true;
  }

  const int available = transport_ != nullptr ? transport_->available() : 0;
  if (available > 0) {
    const size_t have = static_cast<size_t>(available);
    size_t wanted = limit < have ? limit : have;
    if (bodyActive_ && bodyLengthKnown_) {
      const uint32_t remaining = bodyExpected_ - bodyReceived_;
      if (wanted > remaining) {
        wanted = static_cast<size_t>(remaining);
      }
    }
    const int count = transport_->read(output, wanted);
    if (count < 0) {
      setError(error, errorSize, "recv failed");
      return false;
    }
    received = static_cast<size_t>(count);
    if (bodyActive_ && received != 0) {
      bodyReceived_ += static_cast<uint32_t>(received);
      bodyLastActivityMs_ = millis();
    }
    setError(error, errorSize, "");
    return true;
  }

  // Для TLS проверяем также настоящий FIN нижележащего сокета: реализация
  // WiFiClientSecure может не сбросить connected() после close_notify.
  eof = transport_ == nullptr || !transport_->connected() ||
        (transportTls_ && tlsPeerClosed());
  if (eof) {
    close();
    setError(error, errorSize, "");
    return true;
  }

  // Сломанный сервер или потерянный TLS FIN больше не оставляет Z80 в вечном
  // цикле пустых NET_RECV. Это именно ошибка, а не успешный EOF: неизвестное
  // либо недополученное тело нельзя молча принять как целый файл.
  constexpr uint32_t kHttpBodyIdleTimeoutMs = 30000;
  if (bodyActive_ &&
      static_cast<uint32_t>(millis() - bodyLastActivityMs_) >=
          kHttpBodyIdleTimeoutMs) {
    close();
    setError(error, errorSize, "body timeout");
    return false;
  }
  setError(error, errorSize, "");
  return true;
}

bool NetClient::tlsPeerClosed() const {
  const int socket = secureClient_.fd();
  if (socket < 0) {
    return true;
  }
  uint8_t probe = 0;
  const int result = recv(socket, &probe, sizeof(probe),
                          MSG_PEEK | MSG_DONTWAIT);
  // Ноль — корректный TCP EOF. Остальные ошибки здесь не объявляем успехом:
  // тайм-аут тела выше превратит застрявший канал в явный отказ.
  return result == 0;
}

bool NetClient::readHttpHeader(uint16_t& statusCode, uint32_t& contentLength,
                               char* error, size_t errorSize) {
  if (transport_ == nullptr) {
    setError(error, errorSize, "not open");
    return false;
  }
  char* header = httpHeader_;
  redirectLocation_[0] = 0;
  size_t used = 0;
  const uint32_t started = millis();
  int bodyAt = -1;

  while (static_cast<uint32_t>(millis() - started) < 10000) {
    while (transport_->available() > 0 && used < sizeof(httpHeader_) - 1) {
      const int value = transport_->read();
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
    if (!transport_->connected() && transport_->available() == 0) {
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
  bodyLengthKnown_ = false;
  const char* firstSpace = strchr(header, ' ');
  if (firstSpace != nullptr) {
    statusCode = static_cast<uint16_t>(strtoul(firstSpace + 1, nullptr, 10));
  }
  bool chunked = false;
  const char* line = header;
  while (line < header + bodyAt) {
    const char* next = strstr(line, "\r\n");
    if (next == nullptr) {
      break;
    }
    constexpr char kLengthName[] = "content-length:";
    constexpr char kEncodingName[] = "transfer-encoding:";
    constexpr char kLocationName[] = "location:";
    if (startsWithIgnoreCase(line, next, kLengthName)) {
      const char* value = line + sizeof(kLengthName) - 1;
      while (value < next && (*value == ' ' || *value == '\t')) {
        ++value;
      }
      char* parsedEnd = nullptr;
      const unsigned long parsed = strtoul(value, &parsedEnd, 10);
      while (parsedEnd < next && (*parsedEnd == ' ' || *parsedEnd == '\t')) {
        ++parsedEnd;
      }
      if (parsedEnd == value || parsedEnd != next) {
        setError(error, errorSize, "bad content length");
        return false;
      }
      contentLength = static_cast<uint32_t>(parsed);
      bodyLengthKnown_ = true;
    } else if (startsWithIgnoreCase(line, next, kEncodingName) &&
               containsIgnoreCase(line + sizeof(kEncodingName) - 1, next,
                                  "chunked")) {
      chunked = true;
    } else if (startsWithIgnoreCase(line, next, kLocationName)) {
      const char* value = line + sizeof(kLocationName) - 1;
      while (value < next && (*value == ' ' || *value == '\t')) {
        ++value;
      }
      const char* valueEnd = next;
      while (valueEnd > value &&
             (valueEnd[-1] == ' ' || valueEnd[-1] == '\t')) {
        --valueEnd;
      }
      const size_t valueLength = static_cast<size_t>(valueEnd - value);
      if (valueLength < sizeof(redirectLocation_)) {
        memcpy(redirectLocation_, value, valueLength);
        redirectLocation_[valueLength] = 0;
      }
    }
    line = next + 2;
  }
  if (chunked && !isRedirectStatus(statusCode)) {
    setError(error, errorSize, "chunked unsupported");
    return false;
  }

  pendingOffset_ = 0;
  pendingLength_ = 0;
  setError(error, errorSize, "");
  return true;
}

bool NetClient::applyRedirect(uint16_t& port, bool& useTls,
                              char* error, size_t errorSize) {
  const char* location = redirectLocation_;
  const char* locationEnd = location + strlen(location);
  const bool wasTls = useTls;
  bool absolute = false;
  const char* authority = nullptr;

  if (startsWithIgnoreCase(location, locationEnd, "https://")) {
    absolute = true;
    useTls = true;
    authority = location + 8;
  } else if (startsWithIgnoreCase(location, locationEnd, "http://")) {
    absolute = true;
    useTls = false;
    authority = location + 7;
  } else if (location[0] == '/' && location[1] == '/') {
    absolute = true;
    authority = location + 2;
  }
  if (wasTls && !useTls) {
    setError(error, errorSize, "redirect tls downgrade");
    return false;
  }

  if (absolute) {
    const char* authorityEnd = authority;
    while (*authorityEnd != 0 && *authorityEnd != '/' &&
           *authorityEnd != '?' && *authorityEnd != '#') {
      ++authorityEnd;
    }
    const char* colon = nullptr;
    for (const char* at = authority; at < authorityEnd; ++at) {
      if (*at == ':') {
        colon = at;
      }
    }
    const char* hostEnd = colon != nullptr ? colon : authorityEnd;
    const size_t hostLength = static_cast<size_t>(hostEnd - authority);
    if (hostLength == 0 || hostLength >= sizeof(httpHost_)) {
      setError(error, errorSize, "redirect host");
      return false;
    }
    memcpy(httpHost_, authority, hostLength);
    httpHost_[hostLength] = 0;

    port = useTls ? 443 : 80;
    if (colon != nullptr) {
      char* portEnd = nullptr;
      const unsigned long parsed = strtoul(colon + 1, &portEnd, 10);
      if (portEnd != authorityEnd || parsed == 0 || parsed > 65535) {
        setError(error, errorSize, "redirect port");
        return false;
      }
      port = static_cast<uint16_t>(parsed);
    }

    if (*authorityEnd == '/') {
      if (snprintf(httpPath_, sizeof(httpPath_), "%s", authorityEnd) >=
          static_cast<int>(sizeof(httpPath_))) {
        setError(error, errorSize, "redirect path");
        return false;
      }
    } else if (*authorityEnd == '?') {
      if (snprintf(httpPath_, sizeof(httpPath_), "/%s", authorityEnd) >=
          static_cast<int>(sizeof(httpPath_))) {
        setError(error, errorSize, "redirect path");
        return false;
      }
    } else {
      snprintf(httpPath_, sizeof(httpPath_), "/");
    }
  } else if (location[0] == '/') {
    if (snprintf(httpPath_, sizeof(httpPath_), "%s", location) >=
        static_cast<int>(sizeof(httpPath_))) {
      setError(error, errorSize, "redirect path");
      return false;
    }
  } else if (location[0] == '?') {
    char* query = strchr(httpPath_, '?');
    if (query != nullptr) {
      *query = 0;
    }
    const size_t used = strlen(httpPath_);
    if (snprintf(httpPath_ + used, sizeof(httpPath_) - used, "%s", location) >=
        static_cast<int>(sizeof(httpPath_) - used)) {
      setError(error, errorSize, "redirect path");
      return false;
    }
  } else {
    char* slash = strrchr(httpPath_, '/');
    const size_t prefix = slash != nullptr
                              ? static_cast<size_t>(slash - httpPath_) + 1
                              : 1;
    httpPath_[prefix] = 0;
    if (snprintf(httpPath_ + prefix, sizeof(httpPath_) - prefix,
                 "%s", location) >=
        static_cast<int>(sizeof(httpPath_) - prefix)) {
      setError(error, errorSize, "redirect path");
      return false;
    }
  }

  char* fragment = strchr(httpPath_, '#');
  if (fragment != nullptr) {
    *fragment = 0;
  }
  if (httpPath_[0] == 0) {
    snprintf(httpPath_, sizeof(httpPath_), "/");
  }
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
  if (host == nullptr || *host == 0 ||
      snprintf(httpHost_, sizeof(httpHost_), "%s", host) >=
          static_cast<int>(sizeof(httpHost_))) {
    setError(error, errorSize, "http host");
    return false;
  }
  if (path == nullptr || *path == 0) {
    path = "/";
  }
  if (snprintf(httpPath_, sizeof(httpPath_), "%s", path) >=
      static_cast<int>(sizeof(httpPath_))) {
    setError(error, errorSize, "http path");
    return false;
  }

  constexpr unsigned kMaxRedirects = 5;
  bool useTls = port == 443;
  for (unsigned redirects = 0;; ++redirects) {
    // Настроенный простой HTTP proxy не умеет дать WiFiClientSecure проверяемый
    // TLS-туннель. HTTPS поэтому всегда идёт с ESP прямо на целевой сервер.
    const bool useProxy = !useTls && proxyEnabled_ && proxyHost_[0] != 0;
    const char* connectHost = useProxy ? proxyHost_ : httpHost_;
    const uint16_t connectPort = useProxy ? proxyPort_ : port;

    if (!(useTls ? openTls(connectHost, connectPort, error, errorSize)
                 : open(connectHost, connectPort, error, errorSize))) {
      return false;
    }

    int length = -1;
    if (useProxy && port != 80 && port != 0) {
      length = snprintf(
          httpRequest_, sizeof(httpRequest_),
          "GET http://%s:%u%s HTTP/1.0\r\n"
          "Host: %s:%u\r\n"
          "Proxy-Authorization: Basic eng6eng=\r\n"
          "User-Agent: ZiFi (ZX Evo)\r\n"
          "Accept: */*\r\n"
          "Accept-Encoding: identity\r\n"
          "Connection: close\r\n\r\n",
          httpHost_, port, httpPath_, httpHost_, port);
    } else if (useProxy) {
      length = snprintf(
          httpRequest_, sizeof(httpRequest_),
          "GET http://%s%s HTTP/1.0\r\n"
          "Host: %s\r\n"
          "Proxy-Authorization: Basic eng6eng=\r\n"
          "User-Agent: ZiFi (ZX Evo)\r\n"
          "Accept: */*\r\n"
          "Accept-Encoding: identity\r\n"
          "Connection: close\r\n\r\n",
          httpHost_, httpPath_, httpHost_);
    } else if (port != (useTls ? 443 : 80) && port != 0) {
      length = snprintf(
          httpRequest_, sizeof(httpRequest_),
          "GET %s HTTP/1.0\r\n"
          "Host: %s:%u\r\n"
          "User-Agent: ZiFi (ZX Evo)\r\n"
          "Accept: */*\r\n"
          "Accept-Encoding: identity\r\n"
          "Connection: close\r\n\r\n",
          httpPath_, httpHost_, port);
    } else {
      length = snprintf(
          httpRequest_, sizeof(httpRequest_),
          "GET %s HTTP/1.0\r\n"
          "Host: %s\r\n"
          "User-Agent: ZiFi (ZX Evo)\r\n"
          "Accept: */*\r\n"
          "Accept-Encoding: identity\r\n"
          "Connection: close\r\n\r\n",
          httpPath_, httpHost_);
    }

    if (length < 0 || static_cast<size_t>(length) >= sizeof(httpRequest_)) {
      close();
      setError(error, errorSize, "request too long");
      return false;
    }
    if (!sendAll(reinterpret_cast<const uint8_t*>(httpRequest_),
                 static_cast<size_t>(length), error, errorSize) ||
        !readHttpHeader(statusCode, contentLength, error, errorSize)) {
      close();
      return false;
    }
    if (!isRedirectStatus(statusCode)) {
      bodyActive_ = true;
      bodyExpected_ = contentLength;
      bodyReceived_ = 0;
      bodyLastActivityMs_ = millis();
      return true;
    }
    if (redirectLocation_[0] == 0) {
      close();
      setError(error, errorSize, "redirect no location");
      return false;
    }
    if (redirects >= kMaxRedirects - 1) {
      close();
      setError(error, errorSize, "too many redirects");
      return false;
    }
    close();
    if (!applyRedirect(port, useTls, error, errorSize)) {
      return false;
    }
  }
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
