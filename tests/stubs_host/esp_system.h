#pragma once

// Сведения о причине перезагрузки на хосте не нужны: симулятор не падает молча,
// он падает под отладчиком со стеком. Значения возвращаются нейтральные.

#include <stdint.h>

typedef enum {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON = 1,
  ESP_RST_SW = 3,
  ESP_RST_PANIC = 4,
} esp_reset_reason_t;

inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
inline uint32_t esp_random() { return 0x5A5A5A5Au; }

inline void esp_fill_random(void* buffer, size_t length) {
  uint8_t* bytes = static_cast<uint8_t*>(buffer);
  for (size_t index = 0; index < length; ++index) {
    bytes[index] = static_cast<uint8_t>(0x5A + index);
  }
}
