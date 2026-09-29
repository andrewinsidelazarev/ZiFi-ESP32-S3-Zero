#pragma once

// Очередь FreeRTOS на хосте: список фиксированных элементов под мьютексом.
// Мост использует её как единственный канал между «ядрами», поэтому важна
// только сохранность порядка и неблокирующее чтение.

#include "freertos/FreeRTOS.h"

struct HostQueue {
  size_t itemSize = 0;
  std::deque<std::vector<uint8_t>> items;
  std::mutex mutex;
};

using QueueHandle_t = HostQueue*;

inline QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t itemSize) {
  auto* queue = new HostQueue();
  queue->itemSize = itemSize;
  return queue;
}

inline void vQueueDelete(QueueHandle_t queue) { delete queue; }

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item,
                             TickType_t) {
  if (queue == nullptr || item == nullptr) {
    return pdFAIL;
  }
  std::lock_guard<std::mutex> guard(queue->mutex);
  std::vector<uint8_t> copy(queue->itemSize);
  std::memcpy(copy.data(), item, queue->itemSize);
  queue->items.push_back(std::move(copy));
  return pdPASS;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t) {
  if (queue == nullptr || item == nullptr) {
    return pdFAIL;
  }
  std::lock_guard<std::mutex> guard(queue->mutex);
  if (queue->items.empty()) {
    return pdFAIL;
  }
  std::memcpy(item, queue->items.front().data(), queue->itemSize);
  queue->items.pop_front();
  return pdPASS;
}

inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
  if (queue == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> guard(queue->mutex);
  return static_cast<UBaseType_t>(queue->items.size());
}

// Ожидание до ticks (1 тик = 1 мс, как на ESP32) с ранним выходом, как только
// в очереди появился элемент. Сам элемент остаётся в очереди.
inline BaseType_t xQueuePeek(QueueHandle_t queue, void* item, TickType_t ticks) {
  if (queue == nullptr || item == nullptr) {
    return pdFAIL;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ticks);
  for (;;) {
    {
      std::lock_guard<std::mutex> guard(queue->mutex);
      if (!queue->items.empty()) {
        std::memcpy(item, queue->items.front().data(), queue->itemSize);
        return pdPASS;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return pdFAIL;
    }
    std::this_thread::yield();
  }
}

inline BaseType_t xQueueOverwrite(QueueHandle_t queue, const void* item) {
  if (queue == nullptr || item == nullptr) {
    return pdFAIL;
  }
  std::lock_guard<std::mutex> guard(queue->mutex);
  queue->items.clear();
  std::vector<uint8_t> copy(queue->itemSize);
  std::memcpy(copy.data(), item, queue->itemSize);
  queue->items.push_back(std::move(copy));
  return pdPASS;
}
