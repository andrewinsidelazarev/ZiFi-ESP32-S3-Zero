#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Текстовая копия кольцевого журнала. Память выделяется преимущественно в
// PSRAM и должна быть освобождена diagnosticLogFreeSnapshot().
struct DiagnosticLogSnapshot {
  uint8_t* data;
  size_t length;
};

// Подготовить постоянный кольцевой файл во flash. Ошибка журнала не должна
// мешать основной работе ZiFi, поэтому вызывающий код получает только false.
bool diagnosticLogBegin();

// Записать одно существенное событие. К каждой строке автоматически
// добавляются uptime, свободная и минимальная RAM, свободная и минимальная
// PSRAM. Формат printf нужен только для короткого описания самого события.
void diagnosticLogEvent(const char* format, ...);

// Собрать записи по порядку от старой к новой. Повреждённая незавершённая
// запись отбрасывается по CRC и в снимок не попадает.
bool diagnosticLogSnapshot(DiagnosticLogSnapshot& snapshot);
void diagnosticLogFreeSnapshot(DiagnosticLogSnapshot& snapshot);

}  // namespace zifi
