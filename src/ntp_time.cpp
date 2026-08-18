#include "zifi/ntp_time.hpp"

#include <stdint.h>
#include <string.h>

namespace zifi {
namespace {

constexpr uint64_t kUnixEpochAt1900 = 2208988800ULL;
constexpr uint64_t kNtpEraSeconds = 1ULL << 32;
constexpr int64_t kSecondsPerDay = 86400;

void putTwoDigits(char* output, unsigned value) {
  output[0] = static_cast<char>('0' + (value / 10) % 10);
  output[1] = static_cast<char>('0' + value % 10);
}

// Целочисленный алгоритм civil_from_days Говарда Хиннанта: одна календарная
// логика проверяется тестом на ПК и работает в прошивке независимо от
// разрядности time_t.
bool civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
  const unsigned yearOfEra =
      (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 -
       dayOfEra / 146096) /
      365;
  year = static_cast<int>(yearOfEra) + static_cast<int>(era * 400);
  const unsigned dayOfYear =
      dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
  day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
  month = static_cast<unsigned>(static_cast<int>(monthPrime) +
                                (monthPrime < 10 ? 3 : -9));
  year += month <= 2;
  return year >= 0 && year <= 9999;
}

}  // namespace

bool formatNtpTimestamp(uint32_t ntpSeconds, int8_t timezoneHours,
                        char output[15]) {
  if (output == nullptr || timezoneHours < -14 || timezoneHours > 14) {
    return false;
  }
  memcpy(output, "00000000000000", 15);

  uint64_t extendedNtp = ntpSeconds;
  if (extendedNtp < kUnixEpochAt1900) {
    extendedNtp += kNtpEraSeconds;
  }
  int64_t localSeconds =
      static_cast<int64_t>(extendedNtp - kUnixEpochAt1900) +
      static_cast<int64_t>(timezoneHours) * 3600;
  int64_t days = localSeconds / kSecondsPerDay;
  int64_t secondsInDay = localSeconds % kSecondsPerDay;
  if (secondsInDay < 0) {
    secondsInDay += kSecondsPerDay;
    --days;
  }

  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!civilFromDays(days, year, month, day)) {
    return false;
  }
  const unsigned hour = static_cast<unsigned>(secondsInDay / 3600);
  const unsigned minute = static_cast<unsigned>((secondsInDay / 60) % 60);
  const unsigned second = static_cast<unsigned>(secondsInDay % 60);

  output[0] = static_cast<char>('0' + (year / 1000) % 10);
  output[1] = static_cast<char>('0' + (year / 100) % 10);
  output[2] = static_cast<char>('0' + (year / 10) % 10);
  output[3] = static_cast<char>('0' + year % 10);
  putTwoDigits(output + 4, month);
  putTwoDigits(output + 6, day);
  putTwoDigits(output + 8, hour);
  putTwoDigits(output + 10, minute);
  putTwoDigits(output + 12, second);
  output[14] = 0;
  return true;
}

}  // namespace zifi
