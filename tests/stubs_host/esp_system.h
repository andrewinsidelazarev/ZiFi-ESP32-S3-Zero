#pragma once

// Сведения о причине перезагрузки на хосте не нужны: симулятор не падает молча,
// он падает под отладчиком со стеком. Значения возвращаются нейтральные.

#include <stdint.h>
#include <random>

typedef enum {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON = 1,
  ESP_RST_SW = 3,
  ESP_RST_PANIC = 4,
} esp_reset_reason_t;

inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
inline uint32_t esp_random() {
  // Настоящая ESP32 возвращает аппаратную энтропию. Константа в заглушке
  // заставляла каждый нативный процесс повторять ServerGuid, SessionId, TreeId
  // и поколения FileId, поэтому Windows считала разные запуски одним
  // продолжающим работать экземпляром SMB. В MSVC std::random_device получает
  // случайные данные от ОС и сохраняет контракт прошивки.
  std::random_device source;
  return static_cast<uint32_t>(source());
}

inline void esp_fill_random(void* buffer, size_t length) {
  uint8_t* bytes = static_cast<uint8_t*>(buffer);
  size_t offset = 0;
  while (offset < length) {
    const uint32_t value = esp_random();
    for (size_t byte = 0; byte < sizeof(value) && offset < length;
         ++byte, ++offset) {
      bytes[offset] = static_cast<uint8_t>(value >> (byte * 8));
    }
  }
}
