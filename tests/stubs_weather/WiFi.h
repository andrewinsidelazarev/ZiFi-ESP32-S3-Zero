#pragma once

// Подмена WiFi.h для host-теста службы погоды: состояние связи задаёт тест.
enum wl_status_t { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 };

struct WiFiStub {
  wl_status_t status() const;
};

extern WiFiStub WiFi;
