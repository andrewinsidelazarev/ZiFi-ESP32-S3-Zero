#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Выполняет один NTP-запрос и сразу форматирует местное время для Z80.
// Результат всегда занимает ровно 14 ASCII-символов YYYYMMDDhhmmss.
bool queryNtp(int8_t timezoneHours, char output[15],
              char* error, size_t errorSize);

}  // namespace zifi
