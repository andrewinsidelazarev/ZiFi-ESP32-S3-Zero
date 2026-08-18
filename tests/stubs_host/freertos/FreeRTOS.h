#pragma once

// Хостовые заглушки FreeRTOS. Серверу нужны ровно три вещи: задержка, создание
// задачи и очередь. Задачи ложатся на std::thread, очереди — на список под
// мьютексом. Приоритеты и привязка к ядру на ПК смысла не имеют и игнорируются.

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using BaseType_t = int;
using UBaseType_t = unsigned int;
using TickType_t = uint32_t;
using TaskHandle_t = void*;

constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdFAIL = 0;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;

inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }

inline void vTaskDelay(TickType_t ticks) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

using TaskFunction_t = void (*)(void*);

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char*,
                                          uint32_t, void* parameters,
                                          UBaseType_t, TaskHandle_t* handle,
                                          BaseType_t) {
  std::thread(task, parameters).detach();
  if (handle != nullptr) {
    *handle = reinterpret_cast<TaskHandle_t>(1);
  }
  return pdPASS;
}

inline void vTaskDelete(TaskHandle_t) {
  // На хосте задача — обычный поток; он завершится сам по возврату из функции.
}
