#pragma once

// PSRAM на хосте — обычная куча. Флаги ёмкости игнорируются: важно лишь то,
// что кэш каталогов и слоты ввода-вывода реально выделяются и освобождаются,
// а значит их учёт и вытеснение проверяются по-настоящему.

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0x0400
#define MALLOC_CAP_8BIT 0x0004
#define MALLOC_CAP_INTERNAL 0x0800
#define MALLOC_CAP_DEFAULT 0x0000

inline void* heap_caps_malloc(size_t size, uint32_t) { return malloc(size); }

inline void* heap_caps_calloc(size_t count, size_t size, uint32_t) {
  return calloc(count, size);
}

inline void heap_caps_free(void* pointer) { free(pointer); }

inline size_t heap_caps_get_free_size(uint32_t) { return 2u * 1024u * 1024u; }

inline size_t heap_caps_get_largest_free_block(uint32_t) {
  return 1u * 1024u * 1024u;
}

inline size_t heap_caps_get_minimum_free_size(uint32_t) {
  return 1u * 1024u * 1024u;
}
