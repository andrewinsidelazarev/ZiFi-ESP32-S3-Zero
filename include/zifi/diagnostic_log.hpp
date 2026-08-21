#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef ZIFI_DIAGNOSTIC_LOG
#define ZIFI_DIAGNOSTIC_LOG 0
#endif

namespace zifi {

// Текстовая копия кольцевого журнала. Память выделяется преимущественно в
// PSRAM и должна быть освобождена diagnosticLogFreeSnapshot().
struct DiagnosticLogSnapshot {
  uint8_t* data;
  size_t length;
};

#if ZIFI_DIAGNOSTIC_LOG

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

#else

// В обычной прошивке журнал выключен на этапе компиляции. Макрос удаляет не
// только вызов, но и его форматную строку с вычислением аргументов. OTA оставляет
// совместимый интерфейс чтения, который явно сообщает, что снимка нет.
inline bool diagnosticLogBegin() { return false; }
#define diagnosticLogEvent(...) ((void)0)
inline bool diagnosticLogFlush() { return false; }
inline bool diagnosticLogSnapshot(DiagnosticLogSnapshot& snapshot) {
  snapshot.data = nullptr;
  snapshot.length = 0;
  return false;
}
inline void diagnosticLogFreeSnapshot(DiagnosticLogSnapshot& snapshot) {
  snapshot.data = nullptr;
  snapshot.length = 0;
}

#endif

}  // namespace zifi
