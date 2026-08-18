#pragma once

// Пролог для нативной сборки libsmb2 компилятором MSVC. Подключается ключом
// /FI, то есть раньше любого исходника.
//
// Windows SDK объявляет макрос interface (это struct для COM), а в libsmb2
// есть поле с таким именем в struct dcerpc_service. Без снятия макроса
// заголовок библиотеки не разбирается вовсе. Снимаем его сразу после того,
// как системные заголовки уже подключены — иначе SDK вернёт его обратно.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#ifdef interface
#undef interface
#endif

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Атрибут «параметр не используется» из GCC-мира: под MSVC он не нужен.
#ifndef _U_
#define _U_
#endif

// POSIX-функции, которых нет в MSVC. libsmb2 берёт их только для начального
// заполнения генератора и имени пользователя по умолчанию, поэтому простые
// реализации полностью достаточны.

#include <stdlib.h>
#include <string.h>

static __inline void srandom(unsigned int seed) { srand(seed); }
static __inline long random(void) { return rand(); }

static __inline int getlogin_r(char* buffer, size_t length) {
  if (buffer == NULL || length == 0) {
    return -1;
  }
  strncpy(buffer, "zx", length - 1);
  buffer[length - 1] = 0;
  return 0;
}

// Версия сборки: в прошивке её задаёт platformio.ini, здесь — пролог.
#ifndef ZIFI_BUILD_VERSION
#define ZIFI_BUILD_VERSION "host-sim"
#endif
