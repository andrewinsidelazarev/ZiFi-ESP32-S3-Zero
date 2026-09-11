// Host-тест службы погоды (src/weather_service.cpp) для tests/test_weather_parse.py.
//
// Настоящий код службы собирается на ПК с подменами из tests/stubs_weather:
// сеть отвечает то, что задал сценарий, часы идут только в delay(). Так
// видно то, чего не проверить по отдельным разборщикам: какие запросы
// уходят, какой справочник спрашивается, что служба запоминает и какие
// байты названия попадают в запись для Z80 (в 0.6.93 до исправления
// название переводилось в CP866 дважды, и кириллица становилась «??»).
//   weather_service_host <каталог фикстур> [сценарий]
//     -> строки «OK сценарий: …» / «FAIL сценарий: …»; код выхода — число FAIL
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include <Arduino.h>                  // заглушки из tests/stubs_weather
#include <WiFi.h>

#include "zifi/config.hpp"
#include "zifi/weather_service.hpp"

// --- подмены Arduino и Wi-Fi -----------------------------------------------------------------
namespace {
uint32_t g_now = 1000;
wl_status_t g_wifi = WL_CONNECTED;
}  // namespace

uint32_t millis() {
  return g_now;
}

void delay(uint32_t ms) {
  g_now += ms;
}

WiFiStub WiFi;

wl_status_t WiFiStub::status() const {
  return g_wifi;
}

namespace {

// --- поддельная сеть ---------------------------------------------------------------------------
struct Reply {
  bool connected;       // false — соединиться не удалось
  uint16_t status;
  std::string body;
};

// Ответы по началу запроса «хост:порт/путь» (подходит самый длинный ключ);
// у ключа очередь ответов, последний повторяется.
std::map<std::string, std::vector<Reply>> g_replies;
std::vector<std::string> g_requests;      // все запросы по порядку
std::string g_fixtures;
int g_failures = 0;

const char kKyivSearch[] =
    "geocoding-api.open-meteo.com:80/v1/search?name=Kyiv&count=1&language=en&format=json";
const char kRomeItSearch[] =
    "geocoding-api.open-meteo.com:80/v1/search?name=Rome&count=1&language=en&format=json"
    "&countryCode=IT";
// «Рим» в UTF-8 и %XX; кириллицу служба ищет с language=ru
const char kRimSearch[] =
    "geocoding-api.open-meteo.com:80/v1/search?name=%D0%A0%D0%B8%D0%BC&count=1&language=ru"
    "&format=json";
const char kUnknownSearch[] =
    "geocoding-api.open-meteo.com:80/v1/search?name=Qwzxplk&count=1&language=en&format=json";
const char kZip00144[] = "api.zippopotam.us:80/IT/00144";
const char kMeteo[] = "api.open-meteo.com:80/v1/forecast?";

void reset() {
  g_replies.clear();
  g_requests.clear();
  g_wifi = WL_CONNECTED;
}

void reply(const std::string& prefix, uint16_t status, const std::string& body) {
  g_replies[prefix].push_back(Reply{true, status, body});
}

std::string fixture(const char* name) {
  const std::string path = g_fixtures + "/" + name;
  std::string data;
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    printf("FAIL fixture: нет файла %s\n", path.c_str());
    ++g_failures;
    return data;
  }
  char buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof(buffer), file)) != 0) {
    data.append(buffer, got);
  }
  fclose(file);
  return data;
}

void check(bool ok, const char* scenario, const std::string& what) {
  printf("%s %s: %s\n", ok ? "OK" : "FAIL", scenario, what.c_str());
  if (!ok) {
    ++g_failures;
  }
}

std::string hex(const void* data, size_t length) {
  static const char kHex[] = "0123456789abcdef";
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  std::string out;
  for (size_t i = 0; i < length; ++i) {
    out += kHex[bytes[i] >> 4];
    out += kHex[bytes[i] & 0x0F];
  }
  return out;
}

std::string hex(const char* text) {
  return hex(text, strlen(text));
}

// Название места из записи для Z80 (CP866 до нуля) в hex.
std::string placeHex(const uint8_t* record) {
  size_t length = 0;
  while (length < zifi::kWeatherPlaceLength && record[zifi::kWrPlace + length] != 0) {
    ++length;
  }
  return hex(record + zifi::kWrPlace, length);
}

std::string requestsText() {
  std::string out = std::to_string(g_requests.size()) + " запрос(ов)";
  for (const std::string& request : g_requests) {
    out += " | " + request;
  }
  return out;
}

bool startsWith(const std::string& text, const char* prefix) {
  return text.compare(0, strlen(prefix), prefix) == 0;
}

// Одна команда WEATHER_GET с таким zifi.ini (ssid нужен разбору ini).
bool get(zifi::WeatherService& service, const std::string& ini, uint8_t* record,
         std::string& error) {
  zifi::IniConfig config;
  char parseError[64] = {};
  const std::string text = "ssid: test\r\n" + ini;
  if (!config.parse(reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                    parseError, sizeof(parseError))) {
    error = std::string("ini: ") + parseError;
    return false;
  }
  char message[96] = {};
  memset(record, 0xEE, zifi::kWeatherRecordSize);
  const bool ok = service.get(config, record, message, sizeof(message));
  error = message;
  return ok;
}

// --- сценарии -------------------------------------------------------------------------------------
void cityEnglish() {
  const char* name = "city_en";
  reset();
  reply(kKyivSearch, 200, fixture("openmeteo_city_kyiv.json"));
  reply(kMeteo, 200, fixture("open_meteo_roma.json"));
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(get(service, "city: Kyiv\r\n", record, error), name, "погода есть " + error);
  check(g_requests.size() == 2 && g_requests[0] == kKyivSearch, name,
        "поиск по-английски: " + requestsText());
  check(g_requests.size() == 2 &&
            g_requests[1].find("latitude=50.4547&longitude=30.5238") != std::string::npos,
        name, "прогноз по координатам найденного места");
  check(placeHex(record) == hex("Kyiv"), name, "название в записи: " + placeHex(record));
  g_requests.clear();
  check(get(service, "city: Kyiv\r\n", record, error) && g_requests.size() == 1 &&
            startsWith(g_requests[0], kMeteo),
        name, "второй раз место известно, только прогноз: " + requestsText());
}

// Кириллица в любой из трёх кодировок ini ищется одним и тем же запросом
// (UTF-8, language=ru), а в запись попадает CP866 — переведённое один раз.
void cityCyrillic(const char* name, const std::string& value) {
  reset();
  reply(kRimSearch, 200, fixture("openmeteo_city_rim_ru.json"));
  reply(kMeteo, 200, fixture("open_meteo_roma.json"));
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(get(service, "city: " + value + "\r\n", record, error), name, "погода есть " + error);
  check(!g_requests.empty() && g_requests[0] == kRimSearch, name,
        "поиск по-русски в UTF-8: " + requestsText());
  check(placeHex(record) == "90a8ac", name, "«Рим» в CP866 (90 a8 ac): " + placeHex(record));
}

void cityCountry() {
  const char* name = "city_country";
  reset();
  reply(kRomeItSearch, 200, fixture("openmeteo_city_rome_it.json"));
  reply(kMeteo, 200, fixture("open_meteo_roma.json"));
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(get(service, "city: Rome\r\ncountry: IT\r\n", record, error), name,
        "погода есть " + error);
  check(!g_requests.empty() && g_requests[0] == kRomeItSearch, name,
        "country: сужает поиск страной: " + requestsText());
  check(placeHex(record) == hex("Rome"), name, "название в записи: " + placeHex(record));
}

void cityNotFound() {
  const char* name = "city_not_found";
  reset();
  reply(kUnknownSearch, 200, fixture("openmeteo_city_none.json"));
  reply(kKyivSearch, 200, fixture("openmeteo_city_kyiv.json"));
  reply(kMeteo, 200, fixture("open_meteo_roma.json"));
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(!get(service, "city: Qwzxplk\r\n", record, error) && error == "city: not found", name,
        "ответ без results — «" + error + "»");
  check(g_requests.size() == 1, name, "один поиск: " + requestsText());
  check(!get(service, "city: Qwzxplk\r\n", record, error) && error == "city: not found" &&
            g_requests.size() == 1,
        name, "повтор без запроса: " + requestsText());
  check(get(service, "city: Kyiv\r\n", record, error) && g_requests.size() == 3 &&
            g_requests[1] == kKyivSearch,
        name, "другое название в ini — новый поиск: " + requestsText());
}

void zipLegacy() {
  const char* name = "zip_legacy";
  reset();
  reply(kZip00144, 200, fixture("zippopotam_it_00144.json"));
  reply(kMeteo, 200, fixture("open_meteo_roma.json"));
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(get(service, "country: IT\r\nzip: 00144\r\n", record, error), name,
        "погода есть " + error);
  check(g_requests.size() == 2 && g_requests[0] == kZip00144, name,
        "без city: — индекс в zippopotam, как в 0.6.92: " + requestsText());
  check(placeHex(record) == hex("Roma"), name, "название в записи: " + placeHex(record));
}

void zipNotFound() {
  const char* name = "zip_not_found";
  reset();
  reply("api.zippopotam.us:80/IT/99999", 404, "{}");
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(!get(service, "country: IT\r\nzip: 99999\r\n", record, error) &&
            error == "zip: not found" && g_requests.size() == 1,
        name, "404 — «" + error + "», " + requestsText());
  check(!get(service, "country: IT\r\nzip: 99999\r\n", record, error) &&
            error == "zip: not found" && g_requests.size() == 1,
        name, "повтор без запроса: " + requestsText());
}

void cityServerBusy() {
  const char* name = "city_503";
  reset();
  reply(kKyivSearch, 503, "busy");
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  const uint32_t started = g_now;
  check(!get(service, "city: Kyiv\r\n", record, error) && error == "city: http 503", name,
        "«" + error + "»");
  check(g_requests.size() == 3 && g_now - started >= 3000, name,
        "три попытки с паузами 1 и 2 с: " + requestsText());
  check(!get(service, "city: Kyiv\r\n", record, error) && g_requests.size() == 6, name,
        "503 не запоминается — следующая команда спрашивает снова: " + requestsText());
}

void noPlaceInIni() {
  const char* name = "no_place";
  reset();
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(!get(service, "", record, error) && error == "no city in ini", name,
        "ни city:, ни zip: — «" + error + "»");
  check(!get(service, "zip: 00144\r\n", record, error) && error == "no city in ini", name,
        "zip: без country: — «" + error + "»");
  check(g_requests.empty(), name, "в сеть не ходили: " + requestsText());
}

void cityWinsOverZip() {
  const char* name = "city_over_zip";
  reset();
  reply("geocoding-api.open-meteo.com:80/v1/search?name=Kyiv&", 200,
        fixture("openmeteo_city_kyiv.json"));
  reply(kMeteo, 200, fixture("open_meteo_roma.json"));
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(get(service, "city: Kyiv\r\ncountry: UA\r\nzip: 00144\r\n", record, error), name,
        "погода есть " + error);
  check(g_requests.size() == 2 &&
            g_requests[0].find("&countryCode=UA") != std::string::npos,
        name, "есть city: — индекс не нужен: " + requestsText());
}

void noWifi() {
  const char* name = "no_wifi";
  reset();
  g_wifi = WL_DISCONNECTED;
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(!get(service, "city: Kyiv\r\n", record, error) && error == "no wifi" &&
            g_requests.empty(),
        name, "«" + error + "», " + requestsText());
}

void meteoErrorKeepsPlace() {
  const char* name = "meteo_503";
  reset();
  reply(kKyivSearch, 200, fixture("openmeteo_city_kyiv.json"));
  reply(kMeteo, 503, "busy");
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(!get(service, "city: Kyiv\r\n", record, error) && error == "meteo: http 503", name,
        "«" + error + "»");
  g_requests.clear();
  g_replies[kMeteo] = {Reply{true, 200, fixture("open_meteo_roma.json")}};
  check(get(service, "city: Kyiv\r\n", record, error) && g_requests.size() == 1 &&
            startsWith(g_requests[0], kMeteo),
        name, "место найдено раньше — только прогноз: " + requestsText());
  check(placeHex(record) == hex("Kyiv"), name, "название в записи: " + placeHex(record));
}

// У маленьких мест без английского имени геокодер отдаёт местное — например,
// по-украински. Буквы Є є Ї ї Ў ў есть в CP866 и в шрифтах заставок.
void nativeUkrainianName() {
  const char* name = "native_name";
  reset();
  reply(kKyivSearch, 200,
        "{\"results\":[{\"name\":\"Київ\",\"latitude\":50.45,\"longitude\":30.52}]}");
  reply(kMeteo, 200, fixture("open_meteo_roma.json"));
  zifi::NetClient net;
  zifi::WeatherService service(net);
  uint8_t record[zifi::kWeatherRecordSize];
  std::string error;
  check(get(service, "city: Kyiv\r\n", record, error), name, "погода есть " + error);
  check(placeHex(record) == "8aa8f5a2", name, "«Київ» в CP866 (8a a8 f5 a2): " + placeHex(record));
}

struct Scenario {
  const char* name;
  void (*run)();
};

const Scenario kScenarios[] = {
    {"city_en", cityEnglish},
    {"city_utf8", [] { cityCyrillic("city_utf8", "Рим"); }},
    {"city_cp866", [] { cityCyrillic("city_cp866", "\x90\xA8\xAC"); }},
    {"city_cp1251", [] { cityCyrillic("city_cp1251", "\xD0\xE8\xEC"); }},
    {"city_country", cityCountry},
    {"city_not_found", cityNotFound},
    {"zip_legacy", zipLegacy},
    {"zip_not_found", zipNotFound},
    {"city_503", cityServerBusy},
    {"no_place", noPlaceInIni},
    {"city_over_zip", cityWinsOverZip},
    {"no_wifi", noWifi},
    {"meteo_503", meteoErrorKeepsPlace},
    {"native_name", nativeUkrainianName},
};

}  // namespace

// --- подмена сетевого клиента (объявлен в tests/stubs_weather/zifi/net_client.hpp) ----------
namespace zifi {

bool NetClient::httpGet(const char* host, uint16_t port, const char* path,
                        uint16_t& statusCode, uint32_t& contentLength,
                        char* error, size_t errorSize) {
  const std::string request = std::string(host) + ":" + std::to_string(port) + path;
  g_requests.push_back(request);
  g_now += 40;                        // запрос тоже занимает время
  std::vector<Reply>* queue = nullptr;
  size_t best = 0;
  for (auto& entry : g_replies) {
    if (request.compare(0, entry.first.size(), entry.first) == 0 && entry.first.size() >= best) {
      best = entry.first.size();
      queue = &entry.second;
    }
  }
  if (queue == nullptr || queue->empty()) {
    snprintf(error, errorSize, "no route");
    return false;
  }
  const Reply answer = queue->front();
  if (queue->size() > 1) {
    queue->erase(queue->begin());
  }
  if (!answer.connected) {
    snprintf(error, errorSize, "connect failed");
    return false;
  }
  statusCode = answer.status;
  contentLength = static_cast<uint32_t>(answer.body.size());
  body_ = answer.body;
  offset_ = 0;
  return true;
}

bool NetClient::receive(uint8_t* output, size_t limit, size_t& received, bool& eof,
                        char* error, size_t errorSize) {
  const size_t left = body_.size() - offset_;
  received = left < limit ? left : limit;
  if (received > 100) {
    received = 100;                   // по кускам, как из сети
  }
  memcpy(output, body_.data() + offset_, received);
  offset_ += received;
  eof = offset_ >= body_.size();
  if (error != nullptr && errorSize != 0) {
    error[0] = 0;
  }
  return true;
}

void NetClient::close() {
  body_.clear();
  offset_ = 0;
}

}  // namespace zifi

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("FAIL usage: weather_service_host <каталог фикстур> [сценарий]\n");
    return 100;
  }
  g_fixtures = argv[1];
  const char* only = argc > 2 ? argv[2] : nullptr;
  bool ran = false;
  for (const Scenario& scenario : kScenarios) {
    if (only == nullptr || strcmp(only, scenario.name) == 0) {
      scenario.run();
      ran = true;
    }
  }
  if (!ran) {
    printf("FAIL нет сценария %s\n", only);
    return 100;
  }
  return g_failures > 99 ? 99 : g_failures;
}
