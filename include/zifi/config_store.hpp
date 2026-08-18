#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

enum class ConfigSaveResult : uint8_t {
  kUnchanged,
  kWritten,
  kError,
};

// Постоянный кэш исходного zifi.ini в отдельном разделе LittleFS. Во flash
// хранится весь файл, поэтому после reset прошивка сама восстанавливает Wi-Fi,
// а одинаковая конфигурация не вызывает лишних циклов erase/program.
class ConfigStore {
 public:
  static constexpr size_t kMaxIniSize = 1024;

  ConfigStore();

  bool begin(char* error, size_t errorSize);
  bool load(uint8_t* output, size_t capacity, size_t& length,
            char* error, size_t errorSize);
  ConfigSaveResult saveIfChanged(const uint8_t* data, size_t length,
                                 char* error, size_t errorSize);

  bool hasValidConfig() const { return valid_; }
  uint32_t checksum() const { return checksum_; }
  uint16_t storedLength() const { return length_; }

  static uint32_t crc32(const uint8_t* data, size_t length);

 private:
  bool mount();
  bool loadPath(const char* path, uint8_t* output, size_t capacity,
                size_t& length, uint32_t& checksum);
  bool writeNewFile(const uint8_t* data, size_t length, uint32_t checksum,
                    char* error, size_t errorSize);

  bool available_;
  bool valid_;
  uint32_t checksum_;
  uint16_t length_;
};

}  // namespace zifi
