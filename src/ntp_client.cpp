#include "zifi/ntp_client.hpp"

#include <WiFi.h>
#include <WiFiUdp.h>

#include <stdio.h>
#include <string.h>

#include "zifi/ntp_time.hpp"

namespace zifi {
namespace {

void setError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text);
  }
}

}  // namespace

bool queryNtp(int8_t timezoneHours, char output[15],
              char* error, size_t errorSize) {
  if (output == nullptr) {
    return false;
  }
  memcpy(output, "00000000000000", 15);
  if (WiFi.status() != WL_CONNECTED) {
    setError(error, errorSize, "no wifi");
    return false;
  }

  IPAddress address;
  if (WiFi.hostByName("pool.ntp.org", address) != 1) {
    setError(error, errorSize, "ntp dns");
    return false;
  }

  WiFiUDP udp;
  if (!udp.begin(0)) {
    setError(error, errorSize, "ntp udp");
    return false;
  }
  uint8_t request[48] = {};
  request[0] = 0x1B;  // LI=0, версия 3, режим клиента.
  if (!udp.beginPacket(address, 123) || udp.write(request, sizeof(request)) != sizeof(request) ||
      !udp.endPacket()) {
    udp.stop();
    setError(error, errorSize, "ntp send");
    return false;
  }

  const uint32_t started = millis();
  while (udp.parsePacket() < 48) {
    if (static_cast<uint32_t>(millis() - started) >= 3000) {
      udp.stop();
      setError(error, errorSize, "ntp timeout");
      return false;
    }
    delay(2);
    yield();
  }
  uint8_t response[48];
  if (udp.read(response, sizeof(response)) != sizeof(response)) {
    udp.stop();
    setError(error, errorSize, "ntp short");
    return false;
  }
  udp.stop();

  const uint8_t leap = response[0] >> 6;
  const uint8_t mode = response[0] & 0x07;
  if (leap == 3 || (mode != 4 && mode != 5) || response[1] == 0) {
    setError(error, errorSize, "ntp invalid");
    return false;
  }

  const uint32_t ntpSeconds = (static_cast<uint32_t>(response[40]) << 24) |
                              (static_cast<uint32_t>(response[41]) << 16) |
                              (static_cast<uint32_t>(response[42]) << 8) |
                              response[43];
  if (!formatNtpTimestamp(ntpSeconds, timezoneHours, output)) {
    setError(error, errorSize, "ntp year");
    return false;
  }
  setError(error, errorSize, "");
  return true;
}

}  // namespace zifi
