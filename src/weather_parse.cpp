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

// Число (возможно в кавычках, как lat/lon у Nominatim); null считается нулём.
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

// Знаки вне А..я и Ё ё, у которых есть замена в CP866; 0 — замены нет.
// Украинские и белорусские Є є Ї ї Ў ў в CP866 есть (#F2..#F7). І і там
// нет — пишем латинские I i, они выглядят так же; Ґ ґ заменяем на Г г.
// Типографские апостроф, тире и кавычки бывают в названиях OpenStreetMap
// («Кам’янець-Подільський») — их заменяют знаки ASCII.
char cp866Extra(uint32_t code) {
  static const struct {
    uint16_t code;
    uint8_t cp866;
  } kExtra[] = {
      {0x404, 0xF2}, {0x454, 0xF3}, {0x407, 0xF4}, {0x457, 0xF5},   // Є є Ї ї
      {0x40E, 0xF6}, {0x45E, 0xF7},                                 // Ў ў
      {0x406, 'I'}, {0x456, 'i'}, {0x490, 0x83}, {0x491, 0xA3},     // І і Ґ ґ
      {0xB0, 0xF8},                                                 // °
      {0xA0, ' '},                                                  // неразрывный пробел
      {0x2BC, '\''}, {0x2018, '\''}, {0x2019, '\''},                // апострофы
      {0x2013, '-'}, {0x2014, '-'},                                 // тире
      {0xAB, '"'}, {0xBB, '"'}, {0x201C, '"'}, {0x201D, '"'}, {0x201E, '"'},
  };
  for (const auto& extra : kExtra) {
    if (code == extra.code) {
      return static_cast<char>(extra.cp866);
    }
  }
  return 0;
}

// --- кодировки значений zifi.ini ----------------------------------------------------------
// Буква кириллицы по коду CP866 (#80..#FF); 0 — не буква (псевдографика, знаки).
uint32_t cp866Letter(uint8_t code) {
  static const uint16_t kF0[] = {0x401, 0x451, 0x404, 0x454, 0x407, 0x457, 0x40E, 0x45E};
  if (code >= 0x80 && code <= 0xAF) {
    return 0x410 + (code - 0x80);                  // А..п
  }
  if (code >= 0xE0 && code <= 0xEF) {
    return 0x440 + (code - 0xE0);                  // р..я
  }
  if (code >= 0xF0 && code <= 0xF7) {
    return kF0[code - 0xF0];                       // Ё ё Є є Ї ї Ў ў
  }
  return 0;
}

// Буква кириллицы по коду CP1251 (#80..#FF); 0 — не буква.
uint32_t cp1251Letter(uint8_t code) {
  static const struct {
    uint8_t code;
    uint16_t letter;
  } kExtra[] = {
      {0xA8, 0x401}, {0xB8, 0x451}, {0xAA, 0x404}, {0xBA, 0x454},   // Ё ё Є є
      {0xAF, 0x407}, {0xBF, 0x457}, {0xB2, 0x406}, {0xB3, 0x456},   // Ї ї І і
      {0xA5, 0x490}, {0xB4, 0x491}, {0xA1, 0x40E}, {0xA2, 0x45E},   // Ґ ґ Ў ў
  };
  if (code >= 0xC0) {
    return 0x410 + (code - 0xC0);                  // А..я
  }
  for (const auto& extra : kExtra) {
    if (code == extra.code) {
      return extra.letter;
    }
  }
  return 0;
}

// Длина правильной последовательности UTF-8 с первого байта p (1..4);
// 0 — это не UTF-8 (одиночный байт продолжения, обрыв, «длинная» запись).
size_t utf8SequenceLength(const unsigned char* p) {
  if (p[0] < 0x80) {
    return 1;
  }
  size_t count;
  if (p[0] >= 0xC2 && p[0] <= 0xDF) {
    count = 2;
  } else if (p[0] >= 0xE0 && p[0] <= 0xEF) {
    count = 3;
  } else if (p[0] >= 0xF0 && p[0] <= 0xF4) {
    count = 4;
  } else {
    return 0;
  }
  for (size_t i = 1; i < count; ++i) {
    if ((p[i] & 0xC0) != 0x80) {
      return 0;                                    // в том числе конец строки
    }
  }
  if ((p[0] == 0xE0 && p[1] < 0xA0) || (p[0] == 0xED && p[1] >= 0xA0) ||
      (p[0] == 0xF0 && p[1] < 0x90) || (p[0] == 0xF4 && p[1] >= 0x90)) {
    return 0;                                      // «длинная» запись или суррогат
  }
  return count;
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
    } else {
      c = cp866Extra(code);
      if (c == 0) {
        c = latinBase(code);
      }
    }
    out[length++] = c;
  }
  out[length] = 0;
}

void iniTextToUtf8(const char* text, char* out, size_t capacity) {
  if (capacity == 0) {
    return;
  }
  const unsigned char* src = reinterpret_cast<const unsigned char*>(text);
  size_t length = 0;
  // Уже UTF-8 (или чистый ASCII) — копируем целыми буквами.
  bool utf8 = true;
  for (const unsigned char* p = src; *p != 0;) {
    const size_t count = utf8SequenceLength(p);
    if (count == 0) {
      utf8 = false;
      break;
    }
    p += count;
  }
  if (utf8) {
    for (const unsigned char* p = src; *p != 0;) {
      const size_t count = utf8SequenceLength(p);
      if (length + count >= capacity) {
        break;
      }
      memcpy(out + length, p, count);
      length += count;
      p += count;
    }
    out[length] = 0;
    return;
  }
  // Однобайтовая кириллица: CP866 или CP1251 — какая даёт больше букв.
  // В CP866 прописные CP1251 (#C0..#DF) — псевдографика, а в CP1251 почти
  // все буквы CP866 #80..#AF — знаки, поэтому ошибиться трудно. Поровну —
  // CP866, родная кодировка Спектрума.
  size_t letters866 = 0;
  size_t letters1251 = 0;
  for (const unsigned char* p = src; *p != 0; ++p) {
    if (*p >= 0x80) {
      letters866 += cp866Letter(*p) != 0 ? 1 : 0;
      letters1251 += cp1251Letter(*p) != 0 ? 1 : 0;
    }
  }
  const bool cp1251 = letters1251 > letters866;
  for (const unsigned char* p = src; *p != 0; ++p) {
    uint32_t code = *p;
    if (code >= 0x80) {
      code = cp1251 ? cp1251Letter(*p) : cp866Letter(*p);
      if (code == 0) {
        code = '?';
      }
    }
    const size_t count = code < 0x80 ? 1 : 2;      // кириллица в UTF-8 — два байта
    if (length + count >= capacity) {
      break;
    }
    appendUtf8(out, capacity, length, code);
  }
  out[length] = 0;
}

bool parseCitySearch(const char* json, size_t length, GeoResult& out, bool& notFound,
                     char* error, size_t errorSize) {
  const char* end = json + length;
  notFound = false;
  const char* p = skipSpaces(json, end);
  if (p >= end || *p != '{') {
    setError(error, errorSize, "city: broken json");
    return false;
  }
  // Ничего не нашёл — в ответе нет results: {"generationtime_ms":0.4}.
  const char* results = findKey(json, end, "results");
  if (results == nullptr) {
    notFound = true;
    setError(error, errorSize, "city: not found");
    return false;
  }
  if (*results != '[') {
    setError(error, errorSize, "city: broken json");
    return false;
  }
  p = skipSpaces(results + 1, end);
  if (p < end && *p == ']') {
    notFound = true;
    setError(error, errorSize, "city: not found");
    return false;
  }
  // count=1: первое место списка — самое крупное из одноимённых.
  const char* placeEnd = (p < end && *p == '{') ? skipValue(p, end) : nullptr;
  if (placeEnd == nullptr) {
    setError(error, errorSize, "city: broken json");
    return false;
  }
  double latitude = 0.0;
  double longitude = 0.0;
  if (!objectNumber(p, placeEnd, "latitude", latitude) ||
      !objectNumber(p, placeEnd, "longitude", longitude)) {
    setError(error, errorSize, "city: no coordinates");
    return false;
  }
  GeoResult result = {};
  result.latitude = static_cast<float>(latitude);
  result.longitude = static_cast<float>(longitude);
  const char* name = findKey(p, placeEnd, "name");
  if (name == nullptr || !parseString(name, placeEnd, result.place, sizeof(result.place))) {
    result.place[0] = 0;                           // без названия — только погода
  }
  out = result;
  return true;
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

bool parseOpenMeteo(const char* json, size_t length, const char* placeUtf8,
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
  utf8ToCp866(placeUtf8 == nullptr ? "" : placeUtf8,
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
