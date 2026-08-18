#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

constexpr size_t kIniMaxEntries = 16;
constexpr size_t kIniKeySize = 24;
constexpr size_t kIniValueSize = 96;

struct IniEntry {
  char key[kIniKeySize];
  char value[kIniValueSize];
};

class IniConfig {
 public:
  IniConfig();

  bool parse(const uint8_t* data, size_t length, char* error, size_t errorSize);
  const char* get(const char* key, const char* fallback = nullptr) const;
  const char* ssid() const { return get("ssid", ""); }
  const char* password() const { return get("password", ""); }
  int8_t timezoneHours() const;
  size_t count() const { return count_; }
  const IniEntry& entry(size_t index) const { return entries_[index]; }
  void clear();

 private:
  bool set(const char* key, const char* value);

  IniEntry entries_[kIniMaxEntries];
  size_t count_;
};

}  // namespace zifi

