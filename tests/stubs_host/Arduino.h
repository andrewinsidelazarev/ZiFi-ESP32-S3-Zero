#pragma once

// Хостовые заглушки Arduino для нативной сборки SMB-сервера под Windows.
// Цель — гонять НАСТОЯЩИЙ серверный код на ПК: libsmb2, обработчики SMB,
// VfsBridge, VfsClient и двоичный протокол остаются без единой правки, а
// подменяется только «дно» — UART к Z80 и платформенные мелочи.
//
// Почему так: отладка на железе стоит перепрошивки за итерацию, а паника ESP
// не оставляет ни места падения, ни стека — только reset_reason=4. Здесь то же
// самое воспроизводится за секунду и ловится обычным отладчиком.

#include <stddef.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// --- время ------------------------------------------------------------------

inline uint32_t millis() {
  using namespace std::chrono;
  static const steady_clock::time_point start = steady_clock::now();
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(steady_clock::now() - start).count());
}

// Уступить процессор: на ESP это переключение задач FreeRTOS, здесь — потока.
inline void yield() { std::this_thread::yield(); }

inline void delay(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// --- строковый тип Arduino в минимально нужном объёме ------------------------

class String {
 public:
  String() = default;
  String(const char* text) : value_(text == nullptr ? "" : text) {}
  const char* c_str() const { return value_.c_str(); }

 private:
  std::string value_;
};

// --- последовательный порт --------------------------------------------------

// Формат кадра UART железа. Симулятору он безразличен, но подпись begin()
// прошивки обязана совпадать.
#ifndef SERIAL_8N1
#define SERIAL_8N1 0x800001cU
#endif

// Точка врезки симулятора. UartTransport владеет ссылкой на HardwareSerial и
// ничего не знает о том, что на другом конце вместо Z80 стоит эмулятор Wild
// Commander поверх обычной папки. Обмен идёт теми же кадрами протокола.
class HardwareSerial {
 public:
  using Sink = void (*)(void* context, const uint8_t* data, size_t length);

  void begin(unsigned long, uint32_t = 0, int8_t = -1, int8_t = -1,
             bool = false, unsigned long = 20000UL) {}
  void setRxBufferSize(size_t) {}
  void setTxBufferSize(size_t) {}
  void setDebugOutput(bool) {}

  // Приёмная сторона: то, что «Z80» уже прислал в ответ.
  int available() {
    std::lock_guard<std::mutex> guard(mutex_);
    return static_cast<int>(rx_.size());
  }

  int read() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (rx_.empty()) {
      return -1;
    }
    const uint8_t value = rx_.front();
    rx_.pop_front();
    return value;
  }

  size_t read(uint8_t* buffer, size_t length) {
    return readBytes(buffer, length);
  }

  size_t readBytes(uint8_t* buffer, size_t length) {
    std::lock_guard<std::mutex> guard(mutex_);
    size_t taken = 0;
    while (taken < length && !rx_.empty()) {
      buffer[taken++] = rx_.front();
      rx_.pop_front();
    }
    return taken;
  }

  // Передающая сторона: уходит прямо в эмулятор Z80.
  size_t write(const uint8_t* data, size_t length) {
    if (sink_ != nullptr) {
      sink_(sinkContext_, data, length);
    }
    return length;
  }

  size_t write(uint8_t value) { return write(&value, 1); }
  void flush() {}

  // Эмулятор подключается сюда один раз при старте симулятора.
  void attachSink(Sink sink, void* context) {
    sink_ = sink;
    sinkContext_ = context;
  }

  // Эмулятор кладёт сюда свои ответы; UartTransport заберёт их через poll().
  void pushRx(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (size_t index = 0; index < length; ++index) {
      rx_.push_back(data[index]);
    }
  }

 private:
  std::mutex mutex_;
  std::deque<uint8_t> rx_;
  Sink sink_ = nullptr;
  void* sinkContext_ = nullptr;
};

extern HardwareSerial Serial;
extern HardwareSerial Serial0;

// --- прочее из Arduino, что задевает серверный код ---------------------------

class EspClass {
 public:
  // Стабильный «MAC» нужен для устойчивого WSD EndpointReference устройства.
  uint64_t getEfuseMac() const { return 0x00A1B2C3D4E5ULL; }
  uint32_t getFreeHeap() const { return 200u * 1024u; }
  void restart() { std::exit(0); }
};

extern EspClass ESP;

inline bool psramFound() { return true; }
inline size_t ESP_getFreePsram() { return 2u * 1024u * 1024u; }

// MSVC не предоставляет gmtime_r, но имеет gmtime_s с той же гарантией
// потокобезопасности; отличается лишь порядок аргументов.
inline struct tm* gmtime_r(const time_t* timer, struct tm* result) {
  return gmtime_s(result, timer) == 0 ? result : nullptr;
}
