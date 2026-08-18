#pragma once

#include <stdint.h>

namespace zifi {

// Преобразует 32-битные секунды NTP в локальное YYYYMMDDhhmmss. Значения
// после переполнения эпохи NTP в 2036 году трактуются как эра 1.
bool formatNtpTimestamp(uint32_t ntpSeconds, int8_t timezoneHours,
                        char output[15]);

}  // namespace zifi
