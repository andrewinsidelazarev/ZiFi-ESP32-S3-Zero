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
  char place[64];     // UTF-8, название места для экрана
};

// Ответ геокодера Open-Meteo на поиск по названию
// (geocoding-api.open-meteo.com/v1/search?name=…&count=1&language=en|ru):
// координаты и название места на языке запроса («Kyiv», «Рим»; у маленьких
// мест без перевода — местное название). notFound=true — в ответе
// нет results: такого названия геокодер не знает, повтор не поможет.
bool parseCitySearch(const char* json, size_t length, GeoResult& out, bool& notFound,
                     char* error, size_t errorSize);

// Ответ api.zippopotam.us/<страна>/<индекс> (поиск по почтовому индексу,
// ключ zip:): координаты и название места (берётся последнее место списка —
// для итальянских CAP это коммуна).
bool parseZippopotam(const char* json, size_t length, GeoResult& out,
                     char* error, size_t errorSize);

// Значение из zifi.ini -> UTF-8 (в out, с нулём в конце). Файл могли
// сохранить в UTF-8 (редактор на ПК), в CP866 (редактор Wild Commander) или
// в CP1251 (Блокнот Windows «ANSI»). Правильный UTF-8 остаётся как есть,
// иначе берётся та из CP866 и CP1251, в которой больше кириллических букв.
void iniTextToUtf8(const char* text, char* out, size_t capacity);

// Ответ api.open-meteo.com/v1/forecast (current + daily, timeformat=unixtime,
// timezone=auto) -> запись kWeatherRecordSize байт. placeUtf8 — название
// места в UTF-8, как его дал справочник; в CP866 оно переводится только
// здесь, один раз (повторный перевод CP866 превращал кириллицу в «??»).
bool parseOpenMeteo(const char* json, size_t length, const char* placeUtf8,
                    uint8_t* record, char* error, size_t errorSize);

// UTF-8 -> CP866: кириллица переводится (с украинскими и белорусскими
// буквами), латиница с диакритикой упрощается до базовой буквы, типографские
// апостроф, тире и кавычки — до знаков ASCII, остальное заменяется '?'.
// Строка обрезается по capacity.
void utf8ToCp866(const char* text, char* out, size_t capacity);

}  // namespace zifi
