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

// Поставить одно существенное событие в RAM-очередь. К каждой строке
// автоматически добавляются uptime, свободная и минимальная RAM, свободная и
// минимальная PSRAM. Этот вызов не обращается к flash и безопасен во время
// активного UART/VFS-обмена.
void diagnosticLogEvent(const char* format, ...);

// Пакетно перенести RAM-очередь в постоянный кольцевой файл. Вызывать только
// когда SMB/FTP/OTA и UART/VFS простаивают: операции LittleFS временно
// блокируют flash cache обоих ядер.
bool diagnosticLogFlush();

// Собрать записи по порядку от старой к новой. Повреждённая незавершённая
// запись отбрасывается по CRC и в снимок не попадает. Перед чтением RAM-очередь
// автоматически сохраняется; OTA вызывает эту функцию уже без активного VFS.
bool diagnosticLogSnapshot(DiagnosticLogSnapshot& snapshot);
void diagnosticLogFreeSnapshot(DiagnosticLogSnapshot& snapshot);

}  // namespace zifi
