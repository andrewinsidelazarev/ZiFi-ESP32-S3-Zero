#include "zifi/weather_parse.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace zifi {
namespace {

// --- крошечный разбор JSON: ровно то, что нужно двум ответам -----------------------
// Полного дерева не строим: ищем ключи на верхнем уровне объекта, пропуская
// чужие значения целиком. Этого достаточно, потому что структура ответов
// известна заранее, а память ESP дорога.

void setError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text);
  }
}

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

const char* skipSpaces(const char* p, const char* end) {
  while (p < end && isSpace(*p)) {
    ++p;
  }
  return p;
}

// p указывает на открывающую кавычку; вернуть позицию за закрывающей.
const char* skipString(const char* p, const char* end) {
  ++p;
  while (p < end) {
    if (*p == '\\') {
      p += 2;
      continue;
    }
    if (*p == '"') {
      return p + 1;
    }
    ++p;
  }
  return nullptr;
}

// Пропустить любое значение: объект, массив, строку, число или литерал.
const char* skipValue(const char* p, const char* end) {
  p = skipSpaces(p, end);
  if (p >= end) {
    return nullptr;
  }
  if (*p == '"') {
    return skipString(p, end);
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (p < end) {
      if (*p == '"') {
        p = skipString(p, end);
        if (p == nullptr) {
          return nullptr;
        }
        continue;
      }
      if (*p == '{' || *p == '[') {
        ++depth;
      } else if (*p == '}' || *p == ']') {
        --depth;
        if (depth == 0) {
          return p + 1;
        }
      }
      ++p;
    }
    return nullptr;
  }
  while (p < end && *p != ',' && *p != '}' && *p != ']' && !isSpace(*p)) {
    ++p;
  }
  return p;
}

// Найти ключ на верхнем уровне объекта [begin, end) и вернуть указатель на
// его значение; nullptr — ключа нет.
const char* findKey(const char* begin, const char* end, const char* key) {
  const size_t keyLength = strlen(key);
  const char* p = skipSpaces(begin, end);
  if (p < end && *p == '{') {
    ++p;
  }
  while (true) {
    p = skipSpaces(p, end);
    if (p >= end || *p != '"') {
      return nullptr;
    }
    const char* nameEnd = skipString(p, end);
    if (nameEnd == nullptr) {
      return nullptr;
    }
    const bool match = static_cast<size_t>(nameEnd - p) == keyLength + 2 &&
                       memcmp(p + 1, key, keyLength) == 0;
    p = skipSpaces(nameEnd, end);
    if (p >= end || *p != ':') {
      return nullptr;
    }
    p = skipSpaces(p + 1, end);
    if (match) {
      return p;
    }
    p = skipValue(p, end);
    if (p == nullptr) {
      return nullptr;
    }
    p = skipSpaces(p, end);
    if (p < end && *p == ',') {
      ++p;
    }
  }
}

// Число (возможно в кавычках, как у zippopotam); null считается нулём.
bool parseNumber(const char* p, const char* end, double& out, const char** next) {
  p = skipSpaces(p, end);
  if (p >= end) {
    return false;
  }
  bool quoted = false;
  if (*p == '"') {
    quoted = true;
    ++p;
  }
  if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
    out = 0.0;
    p += 4;
  } else {
    char* stop = nullptr;
    out = strtod(p, &stop);
    if (stop == p || stop > end) {
      return false;
    }
    p = stop;
  }
  if (quoted) {
    if (p >= end || *p != '"') {
      return false;
    }
    ++p;
  }
  if (next != nullptr) {
    *next = p;
  }
  return true;
}

void appendUtf8(char* out, size_t capacity, size_t& length, uint32_t code) {
  char buffer[4];
  size_t count;
  if (code < 0x80) {
    buffer[0] = static_cast<char>(code);
    count = 1;
  } else if (code < 0x800) {
    buffer[0] = static_cast<char>(0xC0 | (code >> 6));
    buffer[1] = static_cast<char>(0x80 | (code & 0x3F));
    count = 2;
  } else {
    buffer[0] = static_cast<char>(0xE0 | (code >> 12));
    buffer[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    buffer[2] = static_cast<char>(0x80 | (code & 0x3F));
    count = 3;
  }
  for (size_t i = 0; i < count && length + 1 < capacity; ++i) {
    out[length++] = buffer[i];
  }
}

// Строка в кавычках с раскодированием \", \\, \/ и \uXXXX (в UTF-8).
bool parseString(const char* p, const char* end, char* out, size_t capacity) {
  p = skipSpaces(p, end);
  if (p >= end || *p != '"' || capacity == 0) {
    return false;
  }
  ++p;
  size_t length = 0;
  while (p < end && *p != '"') {
    char c = *p++;
    if (c == '\\' && p < end) {
      c = *p++;
      if (c == 'u' && end - p >= 4) {
        char hex[5] = {p[0], p[1], p[2], p[3], 0};
        appendUtf8(out, capacity, length, static_cast<uint32_t>(strtoul(hex, nullptr, 16)));
        p += 4;
        continue;
      }
      if (c == 'n' || c == 't' || c == 'r') {
        c = ' ';
      }
    }
    if (length + 1 < capacity) {
      out[length++] = c;
    }
  }
  out[length] = 0;
  return p < end;
}

// Массив чисел -> out[0..max); вернуть число прочитанных элементов.
size_t parseNumberArray(const char* p, const char* end, double* out, size_t max) {
  p = skipSpaces(p, end);
  if (p >= end || *p != '[') {
    return 0;
  }
  ++p;
  size_t count = 0;
  while (true) {
    p = skipSpaces(p, end);
    if (p >= end || *p == ']') {
      return count;
    }
    double value = 0.0;
    const char* next = nullptr;
    if (!parseNumber(p, end, value, &next)) {
      return count;
    }
    if (count < max) {
      out[count] = value;
    }
    ++count;
    p = skipSpaces(next, end);
    if (p < end && *p == ',') {
      ++p;
    }
  }
}

bool objectNumber(const char* begin, const char* end, const char* key, double& out) {
  const char* value = findKey(begin, end, key);
  return value != nullptr && parseNumber(value, end, out, nullptr);
}

// --- календарь для unixtime -----------------------------------------------------------
// Дата по числу дней от 1970-01-01 (алгоритм Говарда Хиннанта).
void civilFromDays(int64_t z, int& year, unsigned& month, unsigned& day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year = static_cast<int>(y + (month <= 2 ? 1 : 0));
}

struct LocalTime {
  int year;
  unsigned month;
  unsigned day;
  unsigned weekday;   // 0 — понедельник
  unsigned hour;
  unsigned minute;
};

LocalTime localTime(int64_t unixSeconds, int32_t offsetSeconds) {
  const int64_t local = unixSeconds + offsetSeconds;
  int64_t days = local / 86400;
  int64_t seconds = local - days * 86400;
  if (seconds < 0) {
    seconds += 86400;
    --days;
  }
  LocalTime result;
  civilFromDays(days, result.year, result.month, result.day);
  // 1970-01-01 — четверг, то есть 3 при нумерации с понедельника.
  result.weekday = static_cast<unsigned>(((days + 3) % 7 + 7) % 7);
  result.hour = static_cast<unsigned>(seconds / 3600);
  result.minute = static_cast<unsigned>((seconds % 3600) / 60);
  return result;
}

uint8_t clampU8(double value) {
  const long rounded = lround(value);
  return static_cast<uint8_t>(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded));
}

int8_t clampI8(double value) {
  const long rounded = lround(value);
  return static_cast<int8_t>(rounded < -128 ? -128 : (rounded > 127 ? 127 : rounded));
}

uint16_t clampU16(double value) {
  const long rounded = lround(value);
  return static_cast<uint16_t>(rounded < 0 ? 0 : (rounded > 65535 ? 65535 : rounded));
}

void putU16(uint8_t* at, uint16_t value) {
  at[0] = static_cast<uint8_t>(value);
  at[1] = static_cast<uint8_t>(value >> 8);
}

// Латиница с диакритикой (U+00C0..U+017F) -> базовая буква ASCII.
char latinBase(uint32_t code) {
  static const struct {
    uint32_t first;
    uint32_t last;
    char base;
  } kRanges[] = {
      {0xC0, 0xC5, 'A'}, {0xC7, 0xC7, 'C'}, {0xC8, 0xCB, 'E'}, {0xCC, 0xCF, 'I'},
      {0xD1, 0xD1, 'N'}, {0xD2, 0xD6, 'O'}, {0xD8, 0xD8, 'O'}, {0xD9, 0xDC, 'U'},
      {0xDD, 0xDD, 'Y'}, {0xDF, 0xDF, 's'}, {0xE0, 0xE5, 'a'}, {0xE7, 0xE7, 'c'},
      {0xE8, 0xEB, 'e'}, {0xEC, 0xEF, 'i'}, {0xF1, 0xF1, 'n'}, {0xF2, 0xF6, 'o'},
      {0xF8, 0xF8, 'o'}, {0xF9, 0xFC, 'u'}, {0xFD, 0xFD, 'y'}, {0xFF, 0xFF, 'y'},
      {0x100, 0x105, 'a'}, {0x106, 0x10D, 'c'}, {0x10E, 0x111, 'd'}, {0x112, 0x11B, 'e'},
      {0x11C, 0x123, 'g'}, {0x124, 0x127, 'h'}, {0x128, 0x131, 'i'}, {0x134, 0x135, 'j'},
      {0x136, 0x138, 'k'}, {0x139, 0x142, 'l'}, {0x143, 0x149, 'n'}, {0x14C, 0x151, 'o'},
      {0x154, 0x159, 'r'}, {0x15A, 0x161, 's'}, {0x162, 0x167, 't'}, {0x168, 0x173, 'u'},
      {0x174, 0x175, 'w'}, {0x176, 0x178, 'y'}, {0x179, 0x17E, 'z'},
  };
  for (const auto& range : kRanges) {
    if (code >= range.first && code <= range.last) {
      return range.base;
    }
  }
  return '?';
}

}  // namespace

void utf8ToCp866(const char* text, char* out, size_t capacity) {
  if (capacity == 0) {
    return;
  }
  size_t length = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p != 0 && length + 1 < capacity) {
    uint32_t code;
    if (*p < 0x80) {
      code = *p++;
    } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      code = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      code = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      p += 3;
    } else {
      code = '?';
      ++p;
      while ((*p & 0xC0) == 0x80) {
        ++p;
      }
    }
    char c;
    if (code < 0x80) {
      c = static_cast<char>(code);
    } else if (code >= 0x410 && code <= 0x43F) {
      c = static_cast<char>(0x80 + (code - 0x410));       // А..п
    } else if (code >= 0x440 && code <= 0x44F) {
      c = static_cast<char>(0xE0 + (code - 0x440));       // р..я
    } else if (code == 0x401) {
      c = static_cast<char>(0xF0);                        // Ё
    } else if (code == 0x451) {
      c = static_cast<char>(0xF1);                        // ё
    } else if (code == 0xB0) {
      c = static_cast<char>(0xF8);                        // °
    } else {
      c = latinBase(code);
    }
    out[length++] = c;
  }
  out[length] = 0;
}

bool parseZippopotam(const char* json, size_t length, GeoResult& out,
                     char* error, size_t errorSize) {
  const char* end = json + length;
  const char* places = findKey(json, end, "places");
  if (places == nullptr || *places != '[') {
    setError(error, errorSize, "zip: no places");
    return false;
  }
  const char* p = places + 1;
  bool found = false;
  while (true) {
    p = skipSpaces(p, end);
    if (p >= end || *p == ']') {
      break;
    }
    if (*p != '{') {
      break;
    }
    const char* objectEnd = skipValue(p, end);
    if (objectEnd == nullptr) {
      break;
    }
    GeoResult candidate = {};
    double latitude = 0.0;
    double longitude = 0.0;
    const char* name = findKey(p, objectEnd, "place name");
    if (objectNumber(p, objectEnd, "latitude", latitude) &&
        objectNumber(p, objectEnd, "longitude", longitude) &&
        name != nullptr && parseString(name, objectEnd, candidate.place, sizeof(candidate.place))) {
      candidate.latitude = static_cast<float>(latitude);
      candidate.longitude = static_cast<float>(longitude);
      out = candidate;                  // остаётся последнее место списка
      found = true;
    }
    p = skipSpaces(objectEnd, end);
    if (p < end && *p == ',') {
      ++p;
    }
  }
  if (!found) {
    setError(error, errorSize, "zip: no coordinates");
  }
  return found;
}

bool parseOpenMeteo(const char* json, size_t length, const char* placeCp866,
                    uint8_t* record, char* error, size_t errorSize) {
  const char* end = json + length;
  memset(record, 0, kWeatherRecordSize);
  record[kWrVersion] = kWeatherRecordVersion;

  double offset = 0.0;
  if (!objectNumber(json, end, "utc_offset_seconds", offset)) {
    setError(error, errorSize, "meteo: no utc offset");
    return false;
  }
  const int32_t offsetSeconds = static_cast<int32_t>(offset);

  const char* current = findKey(json, end, "current");
  const char* daily = findKey(json, end, "daily");
  if (current == nullptr || *current != '{' || daily == nullptr || *daily != '{') {
    setError(error, errorSize, "meteo: no current/daily");
    return false;
  }
  const char* currentEnd = skipValue(current, end);
  const char* dailyEnd = skipValue(daily, end);
  if (currentEnd == nullptr || dailyEnd == nullptr) {
    setError(error, errorSize, "meteo: broken json");
    return false;
  }

  double temperature, code, isDay, wind, precipitation, pressure, dataTime;
  if (!objectNumber(current, currentEnd, "temperature_2m", temperature) ||
      !objectNumber(current, currentEnd, "weather_code", code) ||
      !objectNumber(current, currentEnd, "is_day", isDay) ||
      !objectNumber(current, currentEnd, "wind_speed_10m", wind) ||
      !objectNumber(current, currentEnd, "precipitation", precipitation) ||
      !objectNumber(current, currentEnd, "surface_pressure", pressure) ||
      !objectNumber(current, currentEnd, "time", dataTime)) {
    setError(error, errorSize, "meteo: current fields");
    return false;
  }

  // double, а не float: unixtime ~1,8e9 во float (24 бита мантиссы) теряет
  // до двух минут — восход и закат уезжали бы на минуту.
  double times[kWeatherMaxDays], codes[kWeatherMaxDays], tmax[kWeatherMaxDays];
  double tmin[kWeatherMaxDays], sums[kWeatherMaxDays];
  double sunrise[kWeatherMaxDays], sunset[kWeatherMaxDays];
  struct {
    const char* key;
    double* out;
  } arrays[] = {
      {"time", times}, {"weather_code", codes}, {"temperature_2m_max", tmax},
      {"temperature_2m_min", tmin}, {"precipitation_sum", sums},
      {"sunrise", sunrise}, {"sunset", sunset},
  };
  size_t count = kWeatherMaxDays;
  for (const auto& array : arrays) {
    const char* value = findKey(daily, dailyEnd, array.key);
    const size_t got = value == nullptr ? 0 : parseNumberArray(value, dailyEnd, array.out,
                                                                 kWeatherMaxDays);
    if (got == 0) {
      setError(error, errorSize, "meteo: daily fields");
      return false;
    }
    if (got < count) {
      count = got;
    }
  }

  record[kWrStatus] = 1;
  utf8ToCp866(placeCp866 == nullptr ? "" : placeCp866,
              reinterpret_cast<char*>(record + kWrPlace), kWeatherPlaceLength);
  record[kWrTemp] = static_cast<uint8_t>(clampI8(temperature));
  record[kWrCode] = clampU8(code);
  record[kWrIsDay] = isDay != 0.0 ? 1 : 0;
  putU16(record + kWrWind10, clampU16(wind * 10.0));
  putU16(record + kWrPrecip10, clampU16(precipitation * 10.0));
  putU16(record + kWrPressure, clampU16(pressure * 0.750062));    // гПа -> мм рт. ст.
  const LocalTime rise = localTime(static_cast<int64_t>(sunrise[0]), offsetSeconds);
  const LocalTime set = localTime(static_cast<int64_t>(sunset[0]), offsetSeconds);
  const LocalTime now = localTime(static_cast<int64_t>(dataTime), offsetSeconds);
  record[kWrSunrise] = static_cast<uint8_t>(rise.hour);
  record[kWrSunrise + 1] = static_cast<uint8_t>(rise.minute);
  record[kWrSunset] = static_cast<uint8_t>(set.hour);
  record[kWrSunset + 1] = static_cast<uint8_t>(set.minute);
  record[kWrDataTime] = static_cast<uint8_t>(now.hour);
  record[kWrDataTime + 1] = static_cast<uint8_t>(now.minute);
  record[kWrDayCount] = static_cast<uint8_t>(count);
  for (size_t i = 0; i < count; ++i) {
    uint8_t* day = record + kWrDays + i * kWeatherDaySize;
    // Полдень местного дня: unixtime полуночи плюс смещение уже даёт нужную
    // дату, а полдень защищает от округления на границе суток.
    const LocalTime date = localTime(static_cast<int64_t>(times[i]) + 43200, offsetSeconds);
    day[0] = static_cast<uint8_t>(date.day);
    day[1] = static_cast<uint8_t>(date.month);
    day[2] = static_cast<uint8_t>(date.weekday);
    day[3] = clampU8(codes[i]);
    day[4] = static_cast<uint8_t>(clampI8(tmin[i]));
    day[5] = static_cast<uint8_t>(clampI8(tmax[i]));
    putU16(day + 6, clampU16(sums[i] * 10.0));
  }
  return true;
}

}  // namespace zifi
