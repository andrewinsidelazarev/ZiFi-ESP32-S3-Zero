#pragma once

// Wi-Fi на хосте всегда «подключён»: сервер спрашивает только состояние и
// локальный адрес. Слушающий сокет открывает сама libsmb2 через обычный
// системный стек, поэтому подмены сетевого слоя не требуется.

#include <Arduino.h>

constexpr int WL_CONNECTED = 3;

class IPAddress {
 public:
  IPAddress() : octets_{127, 0, 0, 1} {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : octets_{a, b, c, d} {}

  uint8_t operator[](int index) const { return octets_[index & 3]; }

  String toString() const {
    static char text[16];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", octets_[0], octets_[1],
             octets_[2], octets_[3]);
    return String(text);
  }

 private:
  uint8_t octets_[4];
};

// TCP-часть WS-Discovery на хосте не поднимается: к симулятору подключаются
// напрямую по localhost, обнаружение в отладке файловых операций не участвует.
class WiFiClient {
 public:
  operator bool() const { return false; }
  int available() { return 0; }
  int read(uint8_t*, size_t) { return 0; }
  size_t write(const uint8_t*, size_t length) { return length; }
  void stop() {}
  void setNoDelay(bool) {}
  void setTimeout(uint32_t) {}
  bool connected() { return false; }
  IPAddress remoteIP() { return IPAddress(127, 0, 0, 1); }
};

class WiFiServer {
 public:
  WiFiServer() = default;
  explicit WiFiServer(uint16_t) {}
  WiFiServer(uint16_t, uint8_t) {}
  void begin(uint16_t = 0) {}
  void begin(uint16_t, int) {}
  void end() {}
  void stop() {}
  void close() {}
  void setNoDelay(bool) {}
  bool operator!() const { return true; }
  explicit operator bool() const { return false; }
  WiFiClient available() { return WiFiClient(); }
};

class WiFiClassHost {
 public:
  int status() const { return WL_CONNECTED; }
  IPAddress localIP() const { return IPAddress(127, 0, 0, 1); }
  IPAddress subnetMask() const { return IPAddress(255, 255, 255, 0); }
  IPAddress gatewayIP() const { return IPAddress(127, 0, 0, 1); }
};

extern WiFiClassHost WiFi;
