#include "zifi/weather_service.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <stdio.h>
#include <string.h>

namespace zifi {
namespace {

constexpr char kZipHost[] = "api.zippopotam.us";
constexpr char kMeteoHost[] = "api.open-meteo.com";
constexpr uint16_t kHttpPort = 80;
constexpr size_t kBodyCapacity = 4096;     // ответы: ~0,6 КиБ и ~1,2 КиБ
constexpr uint32_t kBodyTimeoutMs = 15000;
// Повторы одного HTTP-запроса: серверы погоды иногда отвечают 503 или рвут
// соединение. Три попытки с паузами 1 и 2 секунды. Новая попытка начинается
// только в первые kWeatherBudgetMs команды WEATHER_GET: одна попытка может
// длиться до ~35 секунд (соединение 8, заголовок 10, тело 15), а плагин ждёт
// ответ 90 секунд — так ответ всегда успевает.
constexpr int kHttpAttempts = 3;
constexpr uint32_t kWeatherBudgetMs = 30000;

void setError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text);
  }
}

// Дописать text в путь URL: буквы, цифры и '-' как есть, остальное — %XX.
// Индексы бывают с пробелом (Нидерланды «1012 AB»), а пробел в строке
// запроса HTTP сломал бы её.
bool appendEncoded(char* out, size_t capacity, size_t& length, const char* text) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
    const bool plain = (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= 'a' && *p <= 'z') || *p == '-';
    const size_t need = plain ? 1 : 3;
    if (length + need >= capacity) {
      return false;
    }
    if (plain) {
      out[length++] = static_cast<char>(*p);
    } else {
      out[length++] = '%';
      out[length++] = kHex[*p >> 4];
      out[length++] = kHex[*p & 0x0F];
    }
  }
  out[length] = 0;
  return true;
}

}  // namespace

WeatherService::WeatherService(NetClient& client)
    : client_(client),
      body_(nullptr),
      bodyLength_(0),
      locationKey_{},
      haveCoordinates_(false),
      latitude_(0.0f),
      longitude_(0.0f),
      place_{},
      deadlineMs_(0) {}

bool WeatherService::ensureBody() {
  if (body_ != nullptr) {
    return true;
  }
  body_ = static_cast<char*>(heap_caps_malloc(kBodyCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body_ == nullptr) {
    body_ = static_cast<char*>(heap_caps_malloc(kBodyCapacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  return body_ != nullptr;
}

bool WeatherService::get(const IniConfig& config, uint8_t* record,
                         char* error, size_t errorSize) {
  const char* country = config.get("country", "");
  const char* zip = config.get("zip", "");
  if (country == nullptr || *country == 0 || zip == nullptr || *zip == 0) {
    setError(error, errorSize, "no country/zip in ini");
    return false;
  }
  char key[sizeof(locationKey_)];
  if (snprintf(key, sizeof(key), "%s/%s", country, zip) >= static_cast<int>(sizeof(key))) {
    setError(error, errorSize, "country/zip too long");
    return false;
  }
  if (strcmp(key, locationKey_) != 0) {
    // Индекс в ini поменяли — прежние координаты больше не годятся.
    snprintf(locationKey_, sizeof(locationKey_), "%s", key);
    haveCoordinates_ = false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setError(error, errorSize, "no wifi");
    return false;
  }
  if (!ensureBody()) {
    setError(error, errorSize, "no memory");
    return false;
  }
  deadlineMs_ = millis() + kWeatherBudgetMs;
  if (!haveCoordinates_ && !geocode(country, zip, error, errorSize)) {
    return false;
  }
  return forecast(record, error, errorSize);
}

// GET host/path по HTTP в body_ с повторами. Повторяется то, что может
// пройти со второго раза: обрыв связи, тайм-аут, ответы 5xx и 429 («слишком
// часто»). Ответ 4xx (например, 404 для неизвестного индекса) повтором не
// исправить — сразу ошибка.
bool WeatherService::download(const char* host, const char* path,
                              char* error, size_t errorSize) {
  // Прогноз идёт после геокодирования: если то съело весь бюджет, не
  // начинаем — плагин повторит команду сам, координаты уже будут известны.
  if (budgetSpent()) {
    setError(error, errorSize, "timeout");
    return false;
  }
  for (int attempt = 1;; ++attempt) {
    bool retryable = false;
    if (downloadOnce(host, path, retryable, error, errorSize)) {
      return true;
    }
    if (!retryable || attempt >= kHttpAttempts || budgetSpent()) {
      return false;                 // в error остаётся причина последней попытки
    }
    delay(1000U * static_cast<uint32_t>(attempt));   // 1 с, потом 2 с
  }
}

// Вышло ли время, отведённое на новые попытки (millis() идёт по кругу,
// поэтому сравнение — через знаковую разность).
bool WeatherService::budgetSpent() const {
  return static_cast<int32_t>(millis() - deadlineMs_) >= 0;
}

// Одна попытка GET host/path по HTTP в body_ (с нулём в конце). Тело длиннее
// буфера — ошибка: усечённый JSON разбирать нельзя. retryable — стоит ли
// пробовать ещё раз.
bool WeatherService::downloadOnce(const char* host, const char* path, bool& retryable,
                                  char* error, size_t errorSize) {
  uint16_t status = 0;
  uint32_t contentLength = 0;
  bodyLength_ = 0;
  retryable = true;                 // обрыв и тайм-аут — повторяемые ошибки
  if (!client_.httpGet(host, kHttpPort, path, status, contentLength, error, errorSize)) {
    return false;
  }
  if (status < 200 || status >= 300) {
    client_.close();
    char text[48];
    snprintf(text, sizeof(text), "http %u", static_cast<unsigned>(status));
    setError(error, errorSize, text);
    retryable = status >= 500 || status == 429;
    return false;
  }
  if (contentLength >= kBodyCapacity) {
    client_.close();
    setError(error, errorSize, "reply too long");
    retryable = false;
    return false;
  }
  const uint32_t started = millis();
  bool eof = false;
  while (!eof) {
    size_t received = 0;
    if (!client_.receive(reinterpret_cast<uint8_t*>(body_) + bodyLength_,
                         kBodyCapacity - 1 - bodyLength_, received, eof, error, errorSize)) {
      client_.close();
      return false;
    }
    bodyLength_ += received;
    if (bodyLength_ >= kBodyCapacity - 1) {
      client_.close();
      setError(error, errorSize, "reply too long");
      retryable = false;
      return false;
    }
    if (contentLength != 0 && bodyLength_ >= contentLength) {
      break;
    }
    if (received == 0) {
      if (static_cast<uint32_t>(millis() - started) > kBodyTimeoutMs) {
        client_.close();
        setError(error, errorSize, "reply timeout");
        return false;
      }
      delay(5);
    }
  }
  client_.close();
  body_[bodyLength_] = 0;
  if (bodyLength_ == 0) {
    setError(error, errorSize, "empty reply");
    return false;
  }
  return true;
}

bool WeatherService::geocode(const char* country, const char* zip,
                             char* error, size_t errorSize) {
  char path[96] = "/";
  size_t length = 1;
  if (!appendEncoded(path, sizeof(path), length, country) || length + 1 >= sizeof(path)) {
    setError(error, errorSize, "zip path too long");
    return false;
  }
  path[length++] = '/';
  path[length] = 0;
  if (!appendEncoded(path, sizeof(path), length, zip)) {
    setError(error, errorSize, "zip path too long");
    return false;
  }
  char text[64];
  if (!download(kZipHost, path, text, sizeof(text))) {
    char message[96];
    snprintf(message, sizeof(message), "zip: %s", text);
    setError(error, errorSize, message);
    return false;
  }
  GeoResult geo = {};
  if (!parseZippopotam(body_, bodyLength_, geo, error, errorSize)) {
    return false;
  }
  latitude_ = geo.latitude;
  longitude_ = geo.longitude;
  utf8ToCp866(geo.place, place_, sizeof(place_));
  haveCoordinates_ = true;
  return true;
}

bool WeatherService::forecast(uint8_t* record, char* error, size_t errorSize) {
  char path[320];
  snprintf(path, sizeof(path),
           "/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,is_day,wind_speed_10m,precipitation,surface_pressure"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,sunrise,sunset"
           "&timezone=auto&forecast_days=%u&timeformat=unixtime",
           static_cast<double>(latitude_), static_cast<double>(longitude_),
           static_cast<unsigned>(kWeatherMaxDays));
  char text[64];
  if (!download(kMeteoHost, path, text, sizeof(text))) {
    char message[96];
    snprintf(message, sizeof(message), "meteo: %s", text);
    setError(error, errorSize, message);
    return false;
  }
  return parseOpenMeteo(body_, bodyLength_, place_, record, error, errorSize);
}

}  // namespace zifi
