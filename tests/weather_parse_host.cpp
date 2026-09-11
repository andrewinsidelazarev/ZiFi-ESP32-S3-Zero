// Host-обёртка для tests/test_weather_parse.py: запускает разбор ответов
// погодных сервисов из src/weather_parse.cpp на ПК и печатает результат.
//   weather_parse_host zip <файл json>            -> GEO <lat> <lon> <place utf-8>
//   weather_parse_host meteo <файл json> <место>  -> REC <hex записи>
//   weather_parse_host cp866 <файл utf-8>         -> CP866 <hex>
// Место с не-ASCII буквами передаётся как @<файл UTF-8>: аргументы командной
// строки Windows приходят в main() в кодовой странице ANSI, а не в UTF-8.
// Этим пользуется модель ESP стенда Unreal (Weather screensaver/tools).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "zifi/weather_parse.hpp"

namespace {

std::vector<char> readFile(const char* path) {
  std::vector<char> data;
  FILE* file = fopen(path, "rb");
  if (file == nullptr) {
    return data;
  }
  char buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof(buffer), file)) != 0) {
    data.insert(data.end(), buffer, buffer + got);
  }
  fclose(file);
  data.push_back(0);
  return data;
}

void printHex(const unsigned char* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    printf("%02x", data[i]);
  }
  printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    printf("ERR usage\n");
    return 2;
  }
  const std::string mode = argv[1];
  std::vector<char> data = readFile(argv[2]);
  if (data.size() <= 1) {
    printf("ERR read %s\n", argv[2]);
    return 2;
  }
  const size_t length = data.size() - 1;
  char error[96] = {};
  if (mode == "zip") {
    zifi::GeoResult geo = {};
    if (!zifi::parseZippopotam(data.data(), length, geo, error, sizeof(error))) {
      printf("ERR %s\n", error);
      return 1;
    }
    printf("GEO %.4f %.4f %s\n", geo.latitude, geo.longitude, geo.place);
    return 0;
  }
  if (mode == "meteo") {
    unsigned char record[zifi::kWeatherRecordSize];
    const char* place = argc > 3 ? argv[3] : "";
    std::vector<char> placeFile;
    if (place[0] == '@') {
      placeFile = readFile(place + 1);
      if (placeFile.empty()) {
        printf("ERR read %s\n", place + 1);
        return 2;
      }
      place = placeFile.data();
    }
    if (!zifi::parseOpenMeteo(data.data(), length, place, record, error, sizeof(error))) {
      printf("ERR %s\n", error);
      return 1;
    }
    printf("REC ");
    printHex(record, sizeof(record));
    return 0;
  }
  if (mode == "cp866") {
    char out[64];
    zifi::utf8ToCp866(data.data(), out, sizeof(out));
    printf("CP866 ");
    printHex(reinterpret_cast<const unsigned char*>(out), strlen(out));
    return 0;
  }
  printf("ERR mode\n");
  return 2;
}
