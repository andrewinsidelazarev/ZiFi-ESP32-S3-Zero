#include "zifi/config.hpp"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace zifi {
namespace {

char lowerAscii(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

bool equalNoCase(const char* left, const char* right) {
  while (*left && *right) {
    if (lowerAscii(*left++) != lowerAscii(*right++)) {
      return false;
    }
  }
  return *left == *right;
}

void setError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text);
  }
}

}  // namespace

IniConfig::IniConfig() : entries_{}, count_(0) {}

void IniConfig::clear() {
  memset(entries_, 0, sizeof(entries_));
  count_ = 0;
}

bool IniConfig::set(const char* key, const char* value) {
  // Повторный ключ заменяет предыдущий. Такое поведение удобно при ручном
  // редактировании INI и совпадает с обычной семантикой «последний победил».
  if (strlen(key) >= kIniKeySize || strlen(value) >= kIniValueSize) {
    return false;
  }
  size_t index = 0;
  while (index < count_ && !equalNoCase(entries_[index].key, key)) {
    ++index;
  }
  if (index == count_) {
    if (count_ == kIniMaxEntries) {
      return false;
    }
    ++count_;
  }
  snprintf(entries_[index].key, sizeof(entries_[index].key), "%s", key);
  snprintf(entries_[index].value, sizeof(entries_[index].value), "%s", value);
  return true;
}

bool IniConfig::parse(const uint8_t* data, size_t length, char* error, size_t errorSize) {
  clear();
  if (data == nullptr && length != 0) {
    setError(error, errorSize, "null ini");
    return false;
  }
  size_t pos = 0;
  if (length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    pos = 3;
  }

  char line[160];
  while (pos < length) {
    size_t lineLength = 0;
    bool lineTooLong = false;
    while (pos < length && data[pos] != '\r' && data[pos] != '\n') {
      if (lineLength + 1 < sizeof(line)) {
        line[lineLength++] = static_cast<char>(data[pos]);
      } else {
        lineTooLong = true;
      }
      ++pos;
    }
    while (pos < length && (data[pos] == '\r' || data[pos] == '\n')) {
      ++pos;
    }
    line[lineLength] = 0;
    if (lineTooLong) {
      // Молчаливое усечение особенно опасно для пароля: Wi-Fi просто не
      // подключится, хотя пользователь видит в файле правильную строку.
      setError(error, errorSize, "ini line too long");
      return false;
    }

    char* begin = line;
    while (*begin == ' ' || *begin == '\t') {
      ++begin;
    }
    if (*begin == 0 || *begin == ';' || *begin == '#') {
      continue;
    }
    char* colon = strchr(begin, ':');
    if (colon == nullptr) {
      continue;
    }
    char* keyEnd = colon;
    while (keyEnd > begin && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) {
      --keyEnd;
    }
    *keyEnd = 0;
    if (*begin == 0) {
      continue;
    }
    for (char* p = begin; *p; ++p) {
      const unsigned char byte = static_cast<unsigned char>(*p);
      if (byte >= 0x80) {
        *begin = 0;
        break;
      }
      *p = lowerAscii(*p);
    }
    if (*begin == 0) {
      continue;
    }

    char* value = colon + 1;
    while (*value == ' ' || *value == '\t') {
      ++value;
    }
    if (*value == '"') {
      ++value;
      char* quote = strchr(value, '"');
      if (quote != nullptr) {
        *quote = 0;
      }
    } else {
      char* end = value + strlen(value);
      while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = 0;
      }
    }
    if (!set(begin, value)) {
      setError(error, errorSize, "too many/long ini keys");
      return false;
    }
  }

  if (ssid()[0] == 0) {
    setError(error, errorSize, "no ssid");
    return false;
  }
  if (strlen(ssid()) > 32) {
    setError(error, errorSize, "ssid too long");
    return false;
  }
  if (strlen(password()) > 63) {
    setError(error, errorSize, "password too long");
    return false;
  }
  setError(error, errorSize, "");
  return true;
}

const char* IniConfig::get(const char* key, const char* fallback) const {
  for (size_t i = 0; i < count_; ++i) {
    if (equalNoCase(entries_[i].key, key)) {
      return entries_[i].value;
    }
  }
  return fallback;
}

int8_t IniConfig::timezoneHours() const {
  const char* value = get("time", "");
  if (value == nullptr || *value == 0) {
    return 0;
  }
  int sign = 1;
  if (*value == '+') {
    ++value;
  } else if (*value == '-') {
    sign = -1;
    ++value;
  }
  if (!isdigit(static_cast<unsigned char>(*value))) {
    // Значения вроде "none" из старых конфигураций означают UTC.
    return 0;
  }
  int hours = 0;
  while (isdigit(static_cast<unsigned char>(*value))) {
    hours = hours * 10 + (*value++ - '0');
  }
  // После числа допустимы пробелы и комментарий из старого zifi.ini. Любой
  // другой хвост считаем ошибочным значением и безопасно оставляем UTC.
  while (*value == ' ' || *value == '\t') {
    ++value;
  }
  if ((*value != 0 && *value != ';' && *value != '#') || hours > 14) {
    return 0;
  }
  return static_cast<int8_t>(sign * hours);
}

}  // namespace zifi
