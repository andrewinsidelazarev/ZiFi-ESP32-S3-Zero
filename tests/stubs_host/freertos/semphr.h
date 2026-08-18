#pragma once

// Мьютексы FreeRTOS на хосте — обычный std::mutex. Кольцевой лог берёт их
// ради атомарности записи, семантика полностью совпадает.

#include "freertos/FreeRTOS.h"

using SemaphoreHandle_t = std::mutex*;

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex(); }
inline void vSemaphoreDelete(SemaphoreHandle_t handle) { delete handle; }

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t) {
  if (handle == nullptr) {
    return pdFAIL;
  }
  handle->lock();
  return pdPASS;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
  if (handle == nullptr) {
    return pdFAIL;
  }
  handle->unlock();
  return pdPASS;
}
