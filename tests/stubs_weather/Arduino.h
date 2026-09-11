#pragma once

// Подмена Arduino.h для host-теста службы погоды (tests/weather_service_host.cpp):
// часы идут только тогда, когда их двигает тест или delay().
#include <stdint.h>

uint32_t millis();
void delay(uint32_t ms);
