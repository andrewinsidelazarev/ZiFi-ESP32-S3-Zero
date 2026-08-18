#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

constexpr size_t kOtaImageHeaderSize = 36;

// Проверяет заголовок именно application-образа ESP32-S3. Одного байта E9
// недостаточно: объединённый factory-образ тоже начинается с него, но не имеет
// esp_app_desc_t сразу после первого заголовка сегмента.
bool isEsp32S3ApplicationImage(const uint8_t* data, size_t length);

}  // namespace zifi
