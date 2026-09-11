#pragma once

// Подмена сетевого клиента для host-теста службы погоды. Каталог
// tests/stubs_weather стоит в /I раньше include, поэтому
// include/zifi/weather_service.hpp получает этот файл вместо настоящего:
// ответы серверов задаёт тест (tests/weather_service_host.cpp), а каждый
// запрос записывается. Методы — ровно те, что зовёт служба погоды.
#include <stddef.h>
#include <stdint.h>

#include <string>

namespace zifi {

class NetClient {
 public:
  bool httpGet(const char* host, uint16_t port, const char* path,
               uint16_t& statusCode, uint32_t& contentLength,
               char* error, size_t errorSize);
  bool receive(uint8_t* output, size_t limit, size_t& received, bool& eof,
               char* error, size_t errorSize);
  void close();

 private:
  std::string body_;                  // тело текущего ответа
  size_t offset_ = 0;                 // сколько уже отдано службе
};

}  // namespace zifi
