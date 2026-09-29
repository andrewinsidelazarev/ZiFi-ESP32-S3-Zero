#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// SHA-1 (FIPS 180-4) без аппаратного ускорения: он нужен и прошивке, и
// тестам на ПК, а данные с SD идут по UART со скоростью ~10 КиБ/с — любой
// программной реализации на ESP32-S3 хватает с огромным запасом.
class Sha1 {
 public:
  static constexpr size_t kDigestSize = 20;

  Sha1() { reset(); }

  void reset();
  void update(const uint8_t* data, size_t length);
  void finish(uint8_t digest[kDigestSize]);

 private:
  void block(const uint8_t* data);

  uint32_t state_[5];
  uint64_t length_;
  uint8_t buffer_[64];
  size_t used_;
};

// GitHub называет файл хэшем его git-объекта: SHA-1 от заголовка
// «blob <длина>\0» и самого содержимого. Тем же хэшем сверяется копия на SD.
void gitBlobBegin(Sha1& sha, uint32_t size);

// 40 шестнадцатеричных цифр ↔ 20 байт. Регистр цифр при разборе не важен.
bool parseSha1Hex(const char* text, uint8_t digest[Sha1::kDigestSize]);
void formatSha1Hex(const uint8_t digest[Sha1::kDigestSize], char text[41]);

}  // namespace zifi
