#pragma once

// Wi-Fi для нативного стенда FTP-сервера: настоящие TCP-сокеты Windows.
//
// Заглушка tests/stubs_host/WiFi.h сетевого слоя не имеет — SMB-стенду он не
// нужен, libsmb2 открывает сокеты сама. FTP-сервер прошивки работает через
// WiFiServer/WiFiClient ядра Arduino, поэтому стенд подставляет их реализацию
// поверх Winsock. Путь tools\host_ftp\stubs стоит в /I раньше tests\stubs_host,
// и прошивочный #include <WiFi.h> находит этот файл.
//
// Поведение повторяет ядро ESP32 там, где на него опирается сервер: копии
// WiFiClient делят один сокет, stop() закрывает его для всех копий, read() без
// данных возвращает -1, write() неблокирующий и может записать часть.

#include <Arduino.h>

#include <memory>

constexpr int WL_CONNECTED = 3;

class IPAddress {
 public:
  IPAddress() : octets_{127, 0, 0, 1} {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : octets_{a, b, c, d} {}

  uint8_t operator[](int index) const { return octets_[index & 3]; }

  // EPRT передаёт адрес текстом.
  bool fromString(const char* text) {
    unsigned parts[4] = {};
    char tail = 0;
    if (text == nullptr ||
        sscanf(text, "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2],
               &parts[3], &tail) != 4) {
      return false;
    }
    for (int index = 0; index < 4; ++index) {
      if (parts[index] > 255) {
        return false;
      }
      octets_[index] = static_cast<uint8_t>(parts[index]);
    }
    return true;
  }

  operator uint32_t() const {
    return static_cast<uint32_t>(octets_[0]) |
           (static_cast<uint32_t>(octets_[1]) << 8) |
           (static_cast<uint32_t>(octets_[2]) << 16) |
           (static_cast<uint32_t>(octets_[3]) << 24);
  }

  String toString() const {
    static char text[16];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", octets_[0], octets_[1],
             octets_[2], octets_[3]);
    return String(text);
  }

 private:
  uint8_t octets_[4];
};

namespace host_net {

struct Socket {
  explicit Socket(SOCKET value) : handle(value) {}
  ~Socket() { close(); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  void close() {
    if (handle != INVALID_SOCKET) {
      closesocket(handle);
      handle = INVALID_SOCKET;
    }
  }

  SOCKET handle;
};

inline void makeNonBlocking(SOCKET handle) {
  u_long enabled = 1;
  ioctlsocket(handle, FIONBIO, &enabled);
}

}  // namespace host_net

class WiFiClient {
 public:
  WiFiClient() = default;
  explicit WiFiClient(SOCKET handle)
      : socket_(std::make_shared<host_net::Socket>(handle)) {
    host_net::makeNonBlocking(handle);
  }

  explicit operator bool() { return connected() != 0; }

  int available() {
    if (!open()) {
      return 0;
    }
    u_long count = 0;
    if (ioctlsocket(socket_->handle, FIONREAD, &count) != 0) {
      return 0;
    }
    return static_cast<int>(count);
  }

  int read() {
    uint8_t value = 0;
    return read(&value, 1) == 1 ? value : -1;
  }

  int read(uint8_t* buffer, size_t length) {
    if (!open() || length == 0) {
      return -1;
    }
    const int received =
        recv(socket_->handle, reinterpret_cast<char*>(buffer),
             static_cast<int>(length), 0);
    return received > 0 ? received : -1;
  }

  size_t write(const uint8_t* data, size_t length) {
    if (!open() || length == 0) {
      return 0;
    }
    const int sent = send(socket_->handle, reinterpret_cast<const char*>(data),
                          static_cast<int>(length), 0);
    return sent > 0 ? static_cast<size_t>(sent) : 0;
  }

  size_t write(uint8_t value) { return write(&value, 1); }

  uint8_t connected() {
    if (!open()) {
      return 0;
    }
    if (available() > 0) {
      return 1;
    }
    char probe = 0;
    const int peeked = recv(socket_->handle, &probe, 1, MSG_PEEK);
    if (peeked > 0) {
      return 1;
    }
    if (peeked == 0) {
      return 0;  // собеседник закрыл соединение
    }
    return WSAGetLastError() == WSAEWOULDBLOCK ? 1 : 0;
  }

  void stop() {
    if (socket_) {
      socket_->close();
    }
    socket_.reset();
  }

  void setNoDelay(bool enabled) {
    if (open()) {
      const BOOL value = enabled ? TRUE : FALSE;
      setsockopt(socket_->handle, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&value), sizeof(value));
    }
  }

  void setTimeout(uint32_t) {}

  int connect(IPAddress address, uint16_t port, int32_t timeoutMs) {
    stop();
    SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
      return 0;
    }
    host_net::makeNonBlocking(handle);
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = static_cast<uint32_t>(address);
    ::connect(handle, reinterpret_cast<const sockaddr*>(&target),
              sizeof(target));
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(handle, &writable);
    timeval wait = {static_cast<long>(timeoutMs / 1000),
                    static_cast<long>((timeoutMs % 1000) * 1000)};
    if (select(0, nullptr, &writable, nullptr, &wait) != 1) {
      closesocket(handle);
      return 0;
    }
    socket_ = std::make_shared<host_net::Socket>(handle);
    return 1;
  }

  IPAddress remoteIP() { return IPAddress(127, 0, 0, 1); }
  IPAddress localIP() { return IPAddress(127, 0, 0, 1); }

 private:
  bool open() const {
    return socket_ && socket_->handle != INVALID_SOCKET;
  }

  std::shared_ptr<host_net::Socket> socket_;
};

class WiFiServer {
 public:
  WiFiServer() = default;
  explicit WiFiServer(uint16_t port, uint8_t = 4) : port_(port) {}
  WiFiServer(const WiFiServer&) = delete;
  WiFiServer& operator=(const WiFiServer&) = delete;
  ~WiFiServer() { stop(); }

  void begin(uint16_t port = 0) {
    stop();
    if (port != 0) {
      port_ = port;
    }
    SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
      return;
    }
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port_);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(handle, reinterpret_cast<const sockaddr*>(&local),
             sizeof(local)) != 0 ||
        listen(handle, 4) != 0) {
      closesocket(handle);
      return;
    }
    host_net::makeNonBlocking(handle);
    listener_ = handle;
  }

  bool hasClient() {
    if (pending_ != INVALID_SOCKET) {
      return true;
    }
    if (listener_ == INVALID_SOCKET) {
      return false;
    }
    pending_ = ::accept(listener_, nullptr, nullptr);
    return pending_ != INVALID_SOCKET;
  }

  WiFiClient accept() {
    if (!hasClient()) {
      return WiFiClient();
    }
    const SOCKET taken = pending_;
    pending_ = INVALID_SOCKET;
    return WiFiClient(taken);
  }

  WiFiClient available() { return accept(); }

  void stop() {
    if (pending_ != INVALID_SOCKET) {
      closesocket(pending_);
      pending_ = INVALID_SOCKET;
    }
    if (listener_ != INVALID_SOCKET) {
      closesocket(listener_);
      listener_ = INVALID_SOCKET;
    }
  }
  void end() { stop(); }
  void close() { stop(); }
  void setNoDelay(bool) {}

  explicit operator bool() const { return listener_ != INVALID_SOCKET; }

 private:
  uint16_t port_ = 0;
  SOCKET listener_ = INVALID_SOCKET;
  SOCKET pending_ = INVALID_SOCKET;
};

class WiFiClassHost {
 public:
  int status() const { return WL_CONNECTED; }
  IPAddress localIP() const { return IPAddress(127, 0, 0, 1); }
  IPAddress subnetMask() const { return IPAddress(255, 255, 255, 0); }
  IPAddress gatewayIP() const { return IPAddress(127, 0, 0, 1); }
};

extern WiFiClassHost WiFi;
