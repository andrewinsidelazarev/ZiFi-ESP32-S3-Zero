#pragma once

// NBNS и WS-Discovery на хосте не нужны: к симулятору подключаются по
// localhost напрямую. Заглушка молчит, но сохраняет контракт вызовов, чтобы
// код обнаружения компилировался и исполнялся без ветвлений «только для ПК».

#include <WiFi.h>

class WiFiUDP {
 public:
  uint8_t begin(uint16_t) { return 1; }
  uint8_t beginMulticast(IPAddress, uint16_t) { return 1; }
  void stop() {}
  int parsePacket() { return 0; }
  int available() { return 0; }
  int read() { return -1; }
  int read(uint8_t*, size_t) { return 0; }
  int read(char*, size_t) { return 0; }
  int beginPacket(IPAddress, uint16_t) { return 1; }
  int beginMulticastPacket() { return 1; }
  int beginPacketMulticast(IPAddress, uint16_t, IPAddress, uint8_t = 1) {
    return 1;
  }
  size_t write(const uint8_t*, size_t length) { return length; }
  int endPacket() { return 1; }
  IPAddress remoteIP() { return IPAddress(127, 0, 0, 1); }
  uint16_t remotePort() { return 0; }
};
