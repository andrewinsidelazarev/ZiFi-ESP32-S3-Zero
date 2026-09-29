// Хост-обвязка src/fat_time.cpp для tests/test_fat_time.py.
//
// Читает команды со stdin построчно и отвечает одной строкой на каждую:
//   unix2fat <unix> <tz>        -> "<date> <time>" либо "range"
//   fat2unix <date> <time> <tz> -> "<unix>" либо "invalid"
//   parse <text>                -> "<unix> <consumed>" либо "bad"
//   format <unix>               -> "YYYYMMDDhhmmss"
//   unix2ft <unix>              -> "<filetime>"
//   ft2unix <filetime>          -> "<unix>" либо "special"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "zifi/fat_time.hpp"

int main() {
  char line[512];
  while (fgets(line, sizeof(line), stdin) != nullptr) {
    line[strcspn(line, "\r\n")] = 0;
    char command[16] = {};
    char rest[480] = {};
    if (sscanf(line, "%15s %479[^\n]", command, rest) < 1) {
      continue;
    }
    if (strcmp(command, "unix2fat") == 0) {
      long long unixSeconds = 0;
      long tz = 0;
      sscanf(rest, "%lld %ld", &unixSeconds, &tz);
      zifi::FatStamp stamp;
      if (zifi::unixToFatStamp(unixSeconds, static_cast<int32_t>(tz), stamp)) {
        printf("%u %u\n", stamp.date, stamp.time);
      } else {
        printf("range\n");
      }
    } else if (strcmp(command, "fat2unix") == 0) {
      unsigned date = 0;
      unsigned time = 0;
      long tz = 0;
      sscanf(rest, "%u %u %ld", &date, &time, &tz);
      zifi::FatStamp stamp;
      stamp.date = static_cast<uint16_t>(date);
      stamp.time = static_cast<uint16_t>(time);
      long long unixSeconds = 0;
      int64_t value = 0;
      if (zifi::fatStampToUnix(stamp, static_cast<int32_t>(tz), value)) {
        unixSeconds = value;
        printf("%lld\n", unixSeconds);
      } else {
        printf("invalid\n");
      }
    } else if (strcmp(command, "parse") == 0) {
      int64_t value = 0;
      const char* end = nullptr;
      if (zifi::parseFtpTimeVal(rest, value, &end)) {
        printf("%lld %d\n", static_cast<long long>(value),
               static_cast<int>(end - rest));
      } else {
        printf("bad\n");
      }
    } else if (strcmp(command, "format") == 0) {
      long long unixSeconds = atoll(rest);
      char output[15];
      zifi::formatFtpTimeVal(unixSeconds, output);
      printf("%s\n", output);
    } else if (strcmp(command, "unix2ft") == 0) {
      long long unixSeconds = atoll(rest);
      printf("%llu\n", static_cast<unsigned long long>(
                           zifi::unixToFileTime(unixSeconds)));
    } else if (strcmp(command, "ft2unix") == 0) {
      unsigned long long fileTime = strtoull(rest, nullptr, 10);
      int64_t value = 0;
      if (zifi::fileTimeToUnix(fileTime, value)) {
        printf("%lld\n", static_cast<long long>(value));
      } else {
        printf("special\n");
      }
    } else {
      printf("unknown\n");
    }
    fflush(stdout);
  }
  return 0;
}
