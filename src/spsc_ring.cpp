#include "zifi/spsc_ring.hpp"

#include <string.h>

namespace zifi {
namespace {

size_t minimum(size_t left, size_t right) {
  return left < right ? left : right;
}

}  // namespace

SpscByteRing::SpscByteRing()
    : storage_(nullptr),
      capacity_(0),
      writePosition_(0),
      readPosition_(0),
      highWater_(0) {}

bool SpscByteRing::attach(uint8_t* storage, size_t capacity) {
  // Разность 32-битных монотонных счётчиков остаётся однозначной, пока
  // ёмкость меньше половины их диапазона.
  if (storage == nullptr || capacity < 2 || capacity > 0x7FFFFFFFUL) {
    return false;
  }
  // Ёмкость обязана быть степенью двойки. Индекс считается как
  // position % capacity_, а позиция — монотонный 32-битный счётчик: при его
  // переполнении остаток сохраняет непрерывность только для степени двойки.
  // Иначе после 4 ГиБ переданных байт индекс скачком уедет и кольцо начнёт
  // молча путать данные. Сейчас используются 64 КиБ и 4 КиБ, но проверка
  // нужна, чтобы произвольный размер не завёл эту мину заново.
  if ((capacity & (capacity - 1)) != 0) {
    return false;
  }
  storage_ = storage;
  capacity_ = static_cast<uint32_t>(capacity);
  reset();
  return true;
}

void SpscByteRing::reset() {
  writePosition_.store(0, std::memory_order_relaxed);
  readPosition_.store(0, std::memory_order_relaxed);
  highWater_.store(0, std::memory_order_relaxed);
}

size_t SpscByteRing::available() const {
  if (!ready()) {
    return 0;
  }
  const uint32_t written = writePosition_.load(std::memory_order_acquire);
  const uint32_t read = readPosition_.load(std::memory_order_acquire);
  const uint32_t used = written - read;
  return used <= capacity_ ? used : capacity_;
}

size_t SpscByteRing::freeSpace() const {
  return ready() ? capacity_ - available() : 0;
}

size_t SpscByteRing::highWaterMark() const {
  return highWater_.load(std::memory_order_relaxed);
}

void SpscByteRing::updateHighWater(uint32_t value) {
  uint32_t previous = highWater_.load(std::memory_order_relaxed);
  while (previous < value &&
         !highWater_.compare_exchange_weak(previous, value,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
  }
}

size_t SpscByteRing::write(const uint8_t* data, size_t length) {
  if (!ready() || data == nullptr || length == 0) {
    return 0;
  }
  const uint32_t written = writePosition_.load(std::memory_order_relaxed);
  const uint32_t read = readPosition_.load(std::memory_order_acquire);
  const uint32_t used = written - read;
  if (used >= capacity_) {
    return 0;
  }
  const size_t count = minimum(length, capacity_ - used);
  const size_t index = written % capacity_;
  const size_t first = minimum(count, capacity_ - index);
  memcpy(storage_ + index, data, first);
  if (first != count) {
    memcpy(storage_, data + first, count - first);
  }
  writePosition_.store(written + static_cast<uint32_t>(count),
                       std::memory_order_release);
  updateHighWater(used + static_cast<uint32_t>(count));
  return count;
}

size_t SpscByteRing::peek(uint8_t* output, size_t capacity) const {
  return peekAt(0, output, capacity);
}

size_t SpscByteRing::peekAt(size_t offset, uint8_t* output,
                            size_t capacity) const {
  if (!ready() || output == nullptr || capacity == 0) {
    return 0;
  }
  const uint32_t read = readPosition_.load(std::memory_order_relaxed);
  const uint32_t written = writePosition_.load(std::memory_order_acquire);
  const uint32_t used = written - read;
  const size_t safeUsed = used <= capacity_ ? used : capacity_;
  if (offset >= safeUsed) {
    return 0;
  }
  const size_t count = minimum(capacity, safeUsed - offset);
  if (count == 0) {
    return 0;
  }
  // Позиция чтения не двигается до подтверждения окна Z80. Поэтому источник
  // может последовательно брать фрагменты одного окна прямо из PSRAM-кольца.
  const size_t index = (read + static_cast<uint32_t>(offset)) % capacity_;
  const size_t first = minimum(count, capacity_ - index);
  memcpy(output, storage_ + index, first);
  if (first != count) {
    memcpy(output + first, storage_, count - first);
  }
  return count;
}

size_t SpscByteRing::discard(size_t length) {
  if (!ready() || length == 0) {
    return 0;
  }
  const uint32_t read = readPosition_.load(std::memory_order_relaxed);
  const uint32_t written = writePosition_.load(std::memory_order_acquire);
  const uint32_t used = written - read;
  const size_t count = minimum(length, used <= capacity_ ? used : capacity_);
  readPosition_.store(read + static_cast<uint32_t>(count),
                      std::memory_order_release);
  return count;
}

size_t SpscByteRing::read(uint8_t* output, size_t capacity) {
  const size_t count = peek(output, capacity);
  return discard(count);
}

}  // namespace zifi
