#pragma once

// Подмена esp_heap_caps.h для host-теста службы погоды: обычная куча ПК.
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)

inline void* heap_caps_malloc(size_t size, uint32_t) {
  return malloc(size);
}
