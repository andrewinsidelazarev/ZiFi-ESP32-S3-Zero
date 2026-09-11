#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/config.hpp"
#include "zifi/net_client.hpp"
#include "zifi/weather_parse.hpp"

namespace zifi {

// Погода для заставки Wild Commander (команда WEATHER_GET).
//
// Место задаётся в zifi.ini ключами country: и zip:. Индекс превращается в
// координаты запросом к api.zippopotam.us — один раз, пока ключи в ini не
// поменялись. Прогноз по готовым координатам берётся у api.open-meteo.com
// заново при каждой команде: как часто обновлять, решает плагин (при запуске
// заставки и затем раз в час). Неудачный HTTP-запрос (503, обрыв) ESP
// повторяет до трёх раз; новые попытки начинаются только в первые 30 секунд
// команды, чтобы ответ успел до конца ожидания плагина (90 секунд).
// Оба запроса идут по обычному HTTP (порт 80,
// через прокси из ini, если он задан): TLS здесь ничего не защищает, а
// памяти и времени экономит много.
// Все методы вызываются только сетевой задачей ядра 0.
class WeatherService {
 public:
  explicit WeatherService(NetClient& client);

  // Заполнить record (kWeatherRecordSize байт). При ошибке возвращает false
  // и короткий текст причины для доклада Z80.
  bool get(const IniConfig& config, uint8_t* record, char* error, size_t errorSize);

 private:
  bool ensureBody();
  bool download(const char* host, const char* path, char* error, size_t errorSize);
  bool downloadOnce(const char* host, const char* path, bool& retryable,
                    char* error, size_t errorSize);
  bool budgetSpent() const;
  bool geocode(const char* country, const char* zip, char* error, size_t errorSize);
  bool forecast(uint8_t* record, char* error, size_t errorSize);

  NetClient& client_;
  char* body_;                        // тело ответа, PSRAM
  size_t bodyLength_;
  char locationKey_[48];              // «страна/индекс» найденных координат
  bool haveCoordinates_;
  float latitude_;
  float longitude_;
  char place_[kWeatherPlaceLength];   // CP866
  uint32_t deadlineMs_;               // до какого millis() можно повторять запросы
};

}  // namespace zifi
