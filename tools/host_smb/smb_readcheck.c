/*
 * Проверка чтения ресурса целиком: то, что делает Проводник, копируя папку с
 * Ево на ПК. Каждый файл читается до конца и складывается на диск, а сверку
 * содержимого делает вызывающий скрипт. Отдельная программа нужна потому, что
 * Проводник к симулятору не подключить: порт в UNC-пути не указать, а 445
 * занят ядром Windows.
 *
 * Запуск: smb_readcheck.exe <хост:порт> <ресурс> <логин> <пароль> <куда>
 *                          [размер-запроса]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Диагностический хук прошивки вкомпилирован в libsmb2-dcerpc.c, а живёт в
 * diagnostic_log.cpp, которого в клиенте нет. Здесь достаточно пустышки. */
void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b);
void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b) {
  (void)stage;
  (void)a;
  (void)b;
}

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

static char g_names[64][256];
static uint64_t g_sizes[64];
static int g_count;

/* Перечисление корня: берём только обычные файлы. */
static int listRoot(struct smb2_context* smb2) {
  struct smb2dir* dir;
  struct smb2dirent* entry;

  dir = smb2_opendir(smb2, "");
  if (dir == NULL) {
    printf("opendir провален: %s\n", smb2_get_error(smb2));
    return -1;
  }
  g_count = 0;
  while ((entry = smb2_readdir(smb2, dir)) != NULL) {
    if (entry->st.smb2_type != SMB2_TYPE_FILE) {
      continue;
    }
    if (g_count >= 64) {
      break;
    }
    snprintf(g_names[g_count], sizeof(g_names[0]), "%s", entry->name);
    g_sizes[g_count] = entry->st.smb2_size;
    ++g_count;
  }
  smb2_closedir(smb2, dir);
  printf("файлов в корне: %d\n", g_count);
  return 0;
}

int main(int argc, char** argv) {
  struct smb2_context* smb2;
  uint8_t* buffer;
  uint32_t chunk = 65536;
  const char* outDir;
  int index;
  int failures = 0;

  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 6) {
    printf("Использование: smb_readcheck.exe <хост:порт> <ресурс> <логин>"
           " <пароль> <куда> [размер-запроса]\n");
    return 2;
  }
  outDir = argv[5];
  if (argc > 6) {
    chunk = (uint32_t)strtoul(argv[6], NULL, 10);
  }
  buffer = (uint8_t*)malloc(chunk);
  if (buffer == NULL) {
    printf("нет памяти под буфер %u\n", (unsigned)chunk);
    return 2;
  }

  {
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
      printf("WSAStartup провален\n");
      return 2;
    }
  }

  smb2 = smb2_init_context();
  if (smb2 == NULL) {
    printf("smb2_init_context провален\n");
    return 2;
  }
  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_password(smb2, argv[4]);
  if (smb2_connect_share(smb2, argv[1], argv[2], argv[3]) != 0) {
    printf("подключение не удалось: %s\n", smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return 2;
  }
  printf("подключено, размер запроса чтения: %u байт\n", (unsigned)chunk);

  if (listRoot(smb2) != 0) {
    smb2_destroy_context(smb2);
    return 2;
  }

  for (index = 0; index < g_count; ++index) {
    struct smb2fh* handle;
    char path[512];
    FILE* out;
    uint64_t total = 0;
    int got = 0;
    DWORD started;

    printf("--- %s (%llu байт)\n", g_names[index],
           (unsigned long long)g_sizes[index]);
    started = GetTickCount();
    handle = smb2_open(smb2, g_names[index], 0 /* O_RDONLY */);
    if (handle == NULL) {
      printf("    ОТКРЫТЬ НЕ УДАЛОСЬ: %s\n", smb2_get_error(smb2));
      ++failures;
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", outDir, g_names[index]);
    out = fopen(path, "wb");
    if (out == NULL) {
      printf("    не создать %s\n", path);
      smb2_close(smb2, handle);
      ++failures;
      continue;
    }
    while ((got = smb2_read(smb2, handle, buffer, chunk)) > 0) {
      fwrite(buffer, 1, (size_t)got, out);
      total += (uint64_t)got;
    }
    fclose(out);
    smb2_close(smb2, handle);
    if (got < 0) {
      printf("    ОШИБКА ЧТЕНИЯ на смещении %llu: %s\n",
             (unsigned long long)total, smb2_get_error(smb2));
      ++failures;
    }
    printf("    прочитано %llu из %llu за %lu мс\n",
           (unsigned long long)total, (unsigned long long)g_sizes[index],
           (unsigned long)(GetTickCount() - started));
    if (total != g_sizes[index]) {
      printf("    НЕДОЧИТАНО\n");
      ++failures;
    }
  }

  /* Второй проход: два файла читаются вперемежку. Так ведёт себя Проводник,
   * копируя папку несколькими потоками, и так сервер вынужден переоткрывать
   * файл на ненулевом смещении — то есть идти позиционным трактом FILEX. */
  if (g_count >= 2) {
    struct smb2fh* first;
    struct smb2fh* second;
    uint64_t readFirst = 0;
    uint64_t readSecond = 0;
    int a = 1;
    int b = 1;

    printf("\n--- вперемежку: %s и %s\n", g_names[0], g_names[g_count - 1]);
    first = smb2_open(smb2, g_names[0], 0);
    second = smb2_open(smb2, g_names[g_count - 1], 0);
    if (first == NULL || second == NULL) {
      printf("    ОТКРЫТЬ НЕ УДАЛОСЬ: %s\n", smb2_get_error(smb2));
      ++failures;
    } else {
      while (a > 0 || b > 0) {
        if (a > 0) {
          a = smb2_read(smb2, first, buffer, 8192);
          if (a > 0) {
            readFirst += (uint64_t)a;
          }
        }
        if (b > 0) {
          b = smb2_read(smb2, second, buffer, 8192);
          if (b > 0) {
            readSecond += (uint64_t)b;
          }
        }
        if (a < 0 || b < 0) {
          printf("    ОШИБКА ЧТЕНИЯ: %s\n", smb2_get_error(smb2));
          ++failures;
          break;
        }
      }
      printf("    %s: %llu из %llu, %s: %llu из %llu\n",
             g_names[0], (unsigned long long)readFirst,
             (unsigned long long)g_sizes[0], g_names[g_count - 1],
             (unsigned long long)readSecond,
             (unsigned long long)g_sizes[g_count - 1]);
      if (readFirst != g_sizes[0] || readSecond != g_sizes[g_count - 1]) {
        printf("    НЕДОЧИТАНО\n");
        ++failures;
      }
    }
    if (first != NULL) {
      smb2_close(smb2, first);
    }
    if (second != NULL) {
      smb2_close(smb2, second);
    }
  }

  /* Третий проход: чтение с произвольного смещения — позиционный тракт в
   * чистом виде, без предварительного прохода от нуля. */
  if (g_count > 0) {
    struct smb2fh* handle = smb2_open(smb2, g_names[g_count - 1], 0);
    if (handle == NULL) {
      printf("\n--- позиционное чтение: открыть не удалось\n");
      ++failures;
    } else {
      int got;
      const uint64_t where = g_sizes[g_count - 1] / 2;
      smb2_lseek(smb2, handle, (int64_t)where, SEEK_SET, NULL);
      got = smb2_read(smb2, handle, buffer, 4096);
      printf("\n--- позиционное чтение %s со смещения %llu: %d байт\n",
             g_names[g_count - 1], (unsigned long long)where, got);
      if (got <= 0) {
        printf("    ОШИБКА: %s\n", smb2_get_error(smb2));
        ++failures;
      }
      smb2_close(smb2, handle);
    }
  }

  printf("\nитог: файлов %d, сбоев %d\n", g_count, failures);
  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  free(buffer);
  return failures == 0 ? 0 : 1;
}
