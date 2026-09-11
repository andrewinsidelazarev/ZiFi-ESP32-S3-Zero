#pragma once

#include <stddef.h>
#include <stdint.h>

// Разбор ответов погодных сервисов в двоичную запись для Z80.
//
// Модуль не зависит от Arduino: его собирает и host-тест на ПК
// (tests/test_weather_parse.py) на сохранённых ответах серверов.
// Формат записи един для прошивки, плагина (Weather screensaver/src/weather.asm)
// и эталона (Weather screensaver/tools/design.py); менять только вместе.

namespace zifi {

constexpr uint8_t kWeatherRecordVersion = 1;
constexpr size_t kWeatherPlaceLength = 24;
constexpr size_t kWeatherMaxDays = 6;
constexpr size_t kWeatherDaySize = 8;

// Смещения полей записи.
enum WeatherRecordLayout : size_t {
  kWrStatus = 0,      // 1 — данные есть
  kWrVersion = 1,     // kWeatherRecordVersion
  kWrPlace = 2,       // название места, CP866, ноль в конце, 24 байта
  kWrTemp = 26,       // температура, °C, знаковый байт
  kWrCode = 27,       // код погоды WMO
  kWrIsDay = 28,      // 1 — день
  kWrWind10 = 29,     // ветер, км/ч x10, u16
  kWrPrecip10 = 31,   // осадки за час, мм x10, u16
  kWrPressure = 33,   // давление у поверхности, мм рт. ст., u16
  kWrSunrise = 35,    // час, минута
  kWrSunset = 37,     // час, минута
  kWrDataTime = 39,   // местное время данных: час, минута
  kWrDayCount = 41,   // число дневных записей
  kWrDays = 42,       // дневные записи по 8 байт: число, месяц, день недели
                      // (0 — понедельник), код WMO, tmin, tmax, осадки x10 u16
};
constexpr size_t kWeatherRecordSize = kWrDays + kWeatherDaySize * kWeatherMaxDays;

struct GeoResult {
  float latitude;
  float longitude;
  char place[64];     // UTF-8, последнее место списка (для CAP Италии — коммуна)
};

// Ответ api.zippopotam.us/<страна>/<индекс>: координаты и название места.
bool parseZippopotam(const char* json, size_t length, GeoResult& out,
                     char* error, size_t errorSize);

// Ответ api.open-meteo.com/v1/forecast (current + daily, timeformat=unixtime,
// timezone=auto) -> запись kWeatherRecordSize байт. place уже в CP866.
bool parseOpenMeteo(const char* json, size_t length, const char* placeCp866,
                    uint8_t* record, char* error, size_t errorSize);

// UTF-8 -> CP866: кириллица переводится, латиница с диакритикой упрощается до
// базовой буквы, остальное заменяется '?'. Строка обрезается по capacity.
void utf8ToCp866(const char* text, char* out, size_t capacity);

}  // namespace zifi
