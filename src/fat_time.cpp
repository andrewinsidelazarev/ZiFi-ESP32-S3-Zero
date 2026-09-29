#include "zifi/fat_time.hpp"

#include <stdio.h>

namespace zifi {
namespace {

constexpr int64_t kSecondsPerDay = 86400;
// 1970-01-01 в FILETIME: сотни наносекунд от 1601-01-01.
constexpr uint64_t kUnixEpochFileTime = 116444736000000000ULL;
constexpr int64_t kFileTimePerSecond = 10000000;

// Дни от 1970-01-01 по гражданской дате и обратно — алгоритмы days_from_civil
// и civil_from_days Говарда Хиннанта. mktime/timegm здесь не годятся: первый
// зависит от пояса процесса, второго нет ни в newlib ESP-IDF, ни в MSVC.
int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned shiftedMonth = month > 2 ? month - 3 : month + 9;
  const unsigned dayOfYear = (153 * shiftedMonth + 2) / 5 + day - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

void civilFromDays(int64_t days, int& year, int& month, int& day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
  const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 -
                              dayOfEra / 146096) /
                             365;
  const unsigned dayOfYear =
      dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const unsigned shiftedMonth = (5 * dayOfYear + 2) / 153;
  day = static_cast<int>(dayOfYear - (153 * shiftedMonth + 2) / 5 + 1);
  month = static_cast<int>(shiftedMonth < 10 ? shiftedMonth + 3
                                             : shiftedMonth - 9);
  year = static_cast<int>(static_cast<int64_t>(yearOfEra) + era * 400 +
                          (month <= 2 ? 1 : 0));
}

bool leapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonth(int year, int month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
  return month == 2 && leapYear(year) ? 29 : kDays[month - 1];
}

bool validCivil(int year, int month, int day, int hour, int minute,
                int second) {
  return year >= 1 && year <= 9999 && month >= 1 && month <= 12 &&
         day >= 1 && day <= daysInMonth(year, month) && hour >= 0 &&
         hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 &&
         second <= 59;
}

int64_t floorDiv(int64_t value, int64_t divisor) {
  int64_t quotient = value / divisor;
  if (value % divisor != 0 && (value < 0) != (divisor < 0)) {
    --quotient;
  }
  return quotient;
}

bool takeDigits(const char*& cursor, int count, int& value) {
  value = 0;
  for (int index = 0; index < count; ++index) {
    const char digit = cursor[index];
    if (digit < '0' || digit > '9') {
      return false;
    }
    value = value * 10 + (digit - '0');
  }
  cursor += count;
  return true;
}

}  // namespace

int64_t civilToUnix(int year, int month, int day, int hour, int minute,
                    int second) {
  return daysFromCivil(year, static_cast<unsigned>(month),
                       static_cast<unsigned>(day)) *
             kSecondsPerDay +
         static_cast<int64_t>(hour) * 3600 + minute * 60 + second;
}

void unixToCivil(int64_t unixSeconds, int& year, int& month, int& day,
                 int& hour, int& minute, int& second) {
  const int64_t days = floorDiv(unixSeconds, kSecondsPerDay);
  const int64_t rest = unixSeconds - days * kSecondsPerDay;
  civilFromDays(days, year, month, day);
  hour = static_cast<int>(rest / 3600);
  minute = static_cast<int>(rest % 3600 / 60);
  second = static_cast<int>(rest % 60);
}

bool fatStampToCivil(FatStamp stamp, int& year, int& month, int& day,
                     int& hour, int& minute, int& second) {
  if (!stamp.known()) {
    return false;
  }
  year = 1980 + (stamp.date >> 9);
  month = (stamp.date >> 5) & 0x0F;
  day = stamp.date & 0x1F;
  hour = stamp.time >> 11;
  minute = (stamp.time >> 5) & 0x3F;
  second = (stamp.time & 0x1F) * 2;
  return validCivil(year, month, day, hour, minute, second);
}

bool fatStampToUnix(FatStamp stamp, int32_t timezoneSeconds,
                    int64_t& unixSeconds) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!fatStampToCivil(stamp, year, month, day, hour, minute, second)) {
    return false;
  }
  unixSeconds =
      civilToUnix(year, month, day, hour, minute, second) - timezoneSeconds;
  return true;
}

bool unixToFatStamp(int64_t unixSeconds, int32_t timezoneSeconds,
                    FatStamp& stamp) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  unixToCivil(unixSeconds + timezoneSeconds, year, month, day, hour, minute,
              second);
  if (year < 1980 || year > 2107) {
    return false;
  }
  stamp.date = static_cast<uint16_t>(((year - 1980) << 9) | (month << 5) |
                                     day);
  stamp.time = static_cast<uint16_t>((hour << 11) | (minute << 5) |
                                     (second / 2));
  return true;
}

bool parseFtpTimeVal(const char* text, int64_t& unixSeconds,
                     const char** end) {
  if (text == nullptr) {
    return false;
  }
  const char* cursor = text;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!takeDigits(cursor, 4, year) || !takeDigits(cursor, 2, month) ||
      !takeDigits(cursor, 2, day) || !takeDigits(cursor, 2, hour) ||
      !takeDigits(cursor, 2, minute) || !takeDigits(cursor, 2, second)) {
    return false;
  }
  if (*cursor == '.') {
    ++cursor;
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    while (*cursor >= '0' && *cursor <= '9') {
      ++cursor;
    }
  }
  if (!validCivil(year, month, day, hour, minute, second)) {
    return false;
  }
  unixSeconds = civilToUnix(year, month, day, hour, minute, second);
  if (end != nullptr) {
    *end = cursor;
  }
  return true;
}

void formatFtpTimeVal(int64_t unixSeconds, char* output) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  unixToCivil(unixSeconds, year, month, day, hour, minute, second);
  snprintf(output, 15, "%04d%02d%02d%02d%02d%02d", year, month, day, hour,
           minute, second);
}

uint64_t unixToFileTime(int64_t unixSeconds) {
  const int64_t value = unixSeconds * kFileTimePerSecond +
                        static_cast<int64_t>(kUnixEpochFileTime);
  return value > 0 ? static_cast<uint64_t>(value) : 0;
}

bool fileTimeToUnix(uint64_t fileTime, int64_t& unixSeconds) {
  // 0 — «не менять», -1 и -2 — управляющие значения SMB SET_INFO.
  if (fileTime == 0 || fileTime >= 0xFFFFFFFFFFFFFFFEULL ||
      fileTime < kUnixEpochFileTime) {
    return false;
  }
  unixSeconds = static_cast<int64_t>((fileTime - kUnixEpochFileTime) /
                                     static_cast<uint64_t>(kFileTimePerSecond));
  return true;
}

}  // namespace zifi
