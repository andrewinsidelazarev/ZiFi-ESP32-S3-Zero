#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/config.hpp"
#include "zifi/net_client.hpp"
#include "zifi/weather_parse.hpp"

namespace zifi {

// Погода для заставки Wild Commander (команда WEATHER_GET).
//
// Место задаётся в zifi.ini ключом city: — название по-английски («Kyiv»,
// «Moscow»; можно и по-русски — тогда и на экране оно по-русски), country:
// сужает поиск страной. Его находит геокодер Open-Meteo. Без city: работает
// прежний способ: country: и
// zip: (почтовый индекс) ищутся в справочнике api.zippopotam.us. Место
// ищется один раз, пока ключи в ini не поменялись; место, которого
// справочник не знает, тоже запоминается и повторно не спрашивается.
// Прогноз по готовым координатам берётся у api.open-meteo.com заново при
// каждой команде: как часто обновлять, решает плагин (при запуске заставки и
// затем раз в час). Неудачный HTTP-запрос (503, обрыв) ESP повторяет до трёх
// раз; новые попытки начинаются только в первые 30 секунд команды, чтобы
// ответ успел до конца ожидания плагина (90 секунд).
// Все запросы идут по обычному HTTP (порт 80, через прокси из ini, если он
// задан): TLS здесь ничего не защищает, а памяти и времени экономит много.
// Все методы вызываются только сетевой задачей ядра 0.
class WeatherService {
 public:
  explicit WeatherService(NetClient& client);

  // Заполнить record (kWeatherRecordSize байт). При ошибке возвращает false
  // и короткий текст причины для доклада Z80.
  bool get(const IniConfig& config, uint8_t* record, char* error, size_t errorSize);

 private:
  bool ensureBody();
  bool download(const char* host, const char* path, uint16_t& status,
                char* error, size_t errorSize);
  bool downloadOnce(const char* host, const char* path, uint16_t& status, bool& retryable,
                    char* error, size_t errorSize);
  bool budgetSpent() const;
  bool geocodeCity(const char* city, const char* country, char* error, size_t errorSize);
  bool geocodeZip(const char* country, const char* zip, char* error, size_t errorSize);
  void setPlace(const GeoResult& geo);
  bool forecast(uint8_t* record, char* error, size_t errorSize);

  NetClient& client_;
  char* body_;                        // тело ответа, PSRAM
  size_t bodyLength_;
  char locationKey_[kIniValueSize * 2 + 8];   // «city:страна/название» или «zip:страна/индекс»
  bool haveCoordinates_;
  bool placeUnknown_;                 // справочник не знает место locationKey_
  float latitude_;
  float longitude_;
  char place_[sizeof(GeoResult::place)];   // UTF-8; в CP866 переводит parseOpenMeteo
  uint32_t deadlineMs_;               // до какого millis() можно повторять запросы
};

}  // namespace zifi
