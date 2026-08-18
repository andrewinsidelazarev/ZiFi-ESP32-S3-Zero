#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Однонаправленное кольцо для ровно одного записывающего и одного читающего
// потока.
// Счётчики находятся во внутренней RAM вместе с объектом, а хранилище может
// находиться в PSRAM. Порядок памяти release/acquire гарантирует, что второе
// ядро увидит записанные байты только после публикации новой позиции.
class SpscByteRing {
 public:
  SpscByteRing();

  bool attach(uint8_t* storage, size_t capacity);
  void reset();

  size_t write(const uint8_t* data, size_t length);
  size_t peek(uint8_t* output, size_t capacity) const;
  size_t peekAt(size_t offset, uint8_t* output, size_t capacity) const;
  size_t discard(size_t length);
  size_t read(uint8_t* output, size_t capacity);

  size_t available() const;
  size_t freeSpace() const;
  size_t capacity() const { return capacity_; }
  size_t highWaterMark() const;
  bool ready() const { return storage_ != nullptr && capacity_ != 0; }

 private:
  void updateHighWater(uint32_t value);

  uint8_t* storage_;
  uint32_t capacity_;
  std::atomic<uint32_t> writePosition_;
  std::atomic<uint32_t> readPosition_;
  std::atomic<uint32_t> highWater_;
};

}  // namespace zifi
