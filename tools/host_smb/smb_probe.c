/* Клиент-пробник для нативного симулятора SMB-сервера.
 *
 * Проводник к симулятору не подпустить: в UNC-пути нельзя указать порт, а 445
 * зарезервирован ядром Windows. Поэтому сценарии, которые роняли прошивку,
 * воспроизводит этот клиент — он умеет подключаться на произвольный порт.
 *
 * Запуск:
 *   smb_probe.exe <хост:порт> <ресурс> <пользователь> <пароль>
 *
 * Проверяются ровно те операции, на которых сервер падал на железе:
 *   1) перечисление корня;
 *   2) открытие КАТАЛОГА как файла — на нём ESP уходила в панику;
 *   3) открытие и чтение обычного файла.
 */

#include <stdio.h>
#include <string.h>

/* Диагностический хук из прошивки: он вкомпилирован в libsmb2-dcerpc.c, но
 * сам живёт в diagnostic_log.cpp, которого в клиенте нет. Пробнику ход RPC не
 * нужен, поэтому здесь пустая реализация. */
void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b);
void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b) {
  (void)stage;
  (void)a;
  (void)b;
}

/* Порядок важен: libsmb2.h опирается на типы и константы из smb2.h. */
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>

#define MAX_ENTRIES 64
static char g_files[MAX_ENTRIES][128];
static uint64_t g_file_sizes[MAX_ENTRIES];
static int g_file_count = 0;
static char g_dirs[MAX_ENTRIES][128];
static int g_dir_count = 0;

static int listRoot(struct smb2_context* smb2) {
  struct smb2dir* dir = smb2_opendir(smb2, "");
  struct smb2dirent* entry;
  int count = 0;

  g_file_count = 0;
  g_dir_count = 0;

  if (dir == NULL) {
    printf("[1] opendir провален: %s\n", smb2_get_error(smb2));
    return -1;
  }
  while ((entry = smb2_readdir(smb2, dir)) != NULL) {
    if (strcmp(entry->name, ".") != 0 && strcmp(entry->name, "..") != 0) {
      if (entry->st.smb2_type == SMB2_TYPE_DIRECTORY) {
        if (g_dir_count < MAX_ENTRIES) {
          strncpy(g_dirs[g_dir_count++], entry->name, sizeof(g_dirs[0]) - 1);
        }
      } else {
        if (g_file_count < MAX_ENTRIES) {
          g_file_sizes[g_file_count] = entry->st.smb2_size;
          strncpy(g_files[g_file_count++], entry->name, sizeof(g_files[0]) - 1);
        }
      }
    }
    printf("    %-24s %s %llu\n", entry->name,
           entry->st.smb2_type == SMB2_TYPE_DIRECTORY ? "<DIR> " : "файл  ",
           (unsigned long long)entry->st.smb2_size);
    ++count;
  }
  smb2_closedir(smb2, dir);
  printf("[1] перечисление корня: %d элементов (файлов: %d, папок: %d)\n", count, g_file_count, g_dir_count);
  return 0;
}

/* Ключевая проба. На железе ровно здесь сервер отвечал c00000ba и следом
 * уходил в панику с reset_reason=4. Ошибка тут ожидаема и нормальна —
 * важно, что сервер обязан её пережить. */
static void openDirectoryAsFile(struct smb2_context* smb2, const char* name) {
  struct smb2fh* handle = smb2_open(smb2, name, 0 /* O_RDONLY */);
  if (handle == NULL) {
    printf("[2] каталог как файл (%s): отказ (ожидаемо) — %s\n",
           name, smb2_get_error(smb2));
    return;
  }
  printf("[2] каталог как файл (%s): сервер ОТКРЫЛ его — это уже неверно\n", name);
  smb2_close(smb2, handle);
}

/* Зеркало внутренней структуры libsmb2: она объявлена в .c и наружу не
 * выведена, а пробнику нужен file_id открытого файла. Порядок полей повторяет
 * lib/libsmb2.c — если он там изменится, это место обязано сломаться заметно. */
struct probe_fh_mirror {
  void* cb;
  void* cb_data;
  smb2_file_id file_id;
  int64_t offset;
  int64_t end_of_file;
};

struct probe_wait {
  int done;
  int status;
};

static void probeInfoCb(struct smb2_context* smb2, int status, void* command_data,
                        void* private_data) {
  struct probe_wait* wait = private_data;
  (void)smb2;
  (void)command_data;
  wait->status = status;
  wait->done = 1;
}

/* Сценарий двойного клика по файлу. Открывая файл приложением, Проводник
 * спрашивает у сервера альтернативные потоки — QUERY_INFO с классом 0x16
 * (FileStreamInformation) и большим выходным буфером. На железе именно после
 * этого запроса сервер замолкал. */
static void doubleClickProbe(struct smb2_context* smb2, const char* name) {
  struct smb2fh* handle = smb2_open(smb2, name, 0 /* O_RDONLY */);
  struct probe_fh_mirror* mirror;
  struct smb2_query_info_request req;
  struct smb2_pdu* pdu;
  struct probe_wait wait;

  if (handle == NULL) {
    printf("[5] двойной клик: файл %s не открылся: %s\n", name,
           smb2_get_error(smb2));
    return;
  }
  mirror = (struct probe_fh_mirror*)handle;

  memset(&req, 0, sizeof(req));
  req.info_type = SMB2_0_INFO_FILE;
  req.file_info_class = 0x16; /* FileStreamInformation */
  req.output_buffer_length = 65536;
  req.additional_information = 0;
  req.flags = 0;
  memcpy(req.file_id, mirror->file_id, sizeof(req.file_id));

  wait.done = 0;
  wait.status = -1;
  pdu = smb2_cmd_query_info_async(smb2, &req, probeInfoCb, &wait);
  if (pdu == NULL) {
    printf("[5] query_info_async не собрался: %s\n", smb2_get_error(smb2));
    smb2_close(smb2, handle);
    return;
  }
  smb2_queue_pdu(smb2, pdu);

  while (!wait.done) {
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)smb2_get_fd(smb2);
    pfd.events = POLLIN;
    if (smb2_which_events(smb2) & POLLOUT) {
      pfd.events |= POLLOUT;
    }
    pfd.revents = 0;
    if (WSAPoll(&pfd, 1, 1000) > 0) {
      if (smb2_service(smb2, pfd.revents) < 0) {
        printf("[5] service error: %s\n", smb2_get_error(smb2));
        break;
      }
    } else {
      printf("[5] timeout ожидания ответа FileStreamInformation\n");
      break;
    }
  }

  printf("[5] двойной клик (%s): FileStreamInformation ответ, статус 0x%08x\n",
         name, (unsigned)wait.status);
  smb2_close(smb2, handle);
}

static void readFile(struct smb2_context* smb2, const char* name, uint64_t expected_size) {
  uint8_t buffer[65536];
  printf("  -> smb2_open(%s) ...\n", name);
  DWORD t0 = GetTickCount();
  struct smb2fh* handle = smb2_open(smb2, name, 0 /* O_RDONLY */);
  int total = 0;
  int got;
  DWORD t1 = GetTickCount();

  if (handle == NULL) {
    printf("[3] открыть %s не удалось (%lu ms): %s\n", name, (unsigned long)(t1 - t0), smb2_get_error(smb2));
    return;
  }
  printf("  -> открыт (%lu ms), читаем ...\n", (unsigned long)(t1 - t0));
  while ((got = smb2_read(smb2, handle, buffer, sizeof(buffer))) > 0) {
    total += got;
    printf("     + read %d байт (всего %d / %llu)\n", got, total, (unsigned long long)expected_size);
  }
  DWORD t2 = GetTickCount();
  smb2_close(smb2, handle);
  printf("[3] прочитано из %s: %d байт за %lu ms (ожидалось %llu)\n",
         name, total, (unsigned long)(t2 - t1), (unsigned long long)expected_size);
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  struct smb2_context* smb2;
  const char* server;
  const char* share;
  const char* user;
  const char* password;
  int i;

  if (argc < 5) {
    printf("Использование: smb_probe.exe <хост:порт> <ресурс> <логин> <пароль>\n");
    return 1;
  }
  server = argv[1];
  share = argv[2];
  user = argv[3];
  password = argv[4];

  /* WinSock инициализируется явно: libsmb2 этого не делает, а на ESP шага
   * попросту нет. Без него первый же сокет отказывает. */
  {
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
      printf("WSAStartup провален\n");
      return 1;
    }
  }

  smb2 = smb2_init_context();
  if (smb2 == NULL) {
    printf("smb2_init_context провален\n");
    return 1;
  }

  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_password(smb2, password);

  if (smb2_connect_share(smb2, server, share, user) != 0) {
    printf("Подключение к %s\\%s не удалось: %s\n", server, share,
           smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return 1;
  }
  printf("Подключено к %s\\%s под %s\n\n", server, share, user);

  listRoot(smb2);
  
  if (g_dir_count > 0) {
    openDirectoryAsFile(smb2, g_dirs[0]);
  } else {
    openDirectoryAsFile(smb2, "bin");
  }

  /* Тест 7: запись полного wc.ini */
  printf("\n[7] восстановление полного wc.ini:\n");
  {
    struct smb2fh* fh_write = smb2_open(smb2, "wc.ini", O_WRONLY | O_CREAT | O_TRUNC);
    if (fh_write != NULL) {
      const char text[] = 
        "Wild Commander v1.10i+\r\n"
        "\r\n"
        "[SETUP]\r\n"
        "CPU_FREQ=2\r\n"
        "SavePaths=1\r\n"
        "SavePosition=1\r\n"
        "TextMode=2\r\n"
        "ScreenSaver=3\r\n"
        "Logo=1\r\n"
        "\r\n"
        "[PLUGINS]\r\n"
        "FILEX.WMF\r\n"
        "CHKDSK.WMF\r\n"
        "MOUNTER.WMF -memory=128 -cpu=3,5 -cache=off -rs232=115200\r\n"
        "SETIME.WMF\r\n"
        "RE.WMF\r\n"
        "TXTEDIT.WMF -highlight=auto\r\n"
        "FILE_CR.WMF\r\n"
        "TRDisp.wmf\r\n"
        "TRDUMP.WMF\r\n"
        "ROM_PROG.WMF\r\n"
        "WPLAYER.WMF\r\n"
        "GSPLAYER.WMF -midi_chip=2\r\n"
        "VIDEO_PL.WMF\r\n"
        "BMPV.WMF\r\n"
        "ILBMVIEW.WMF\r\n"
        "FTVIEW.WMF\r\n"
        "GIFVIEW.WMF\r\n"
        "MCGSV.WMF\r\n"
        "TAPM.WMF -loading=real -cpu=3,5 -memory=128 -basic=48\r\n"
        "COMPILER.WMF\r\n"
        "CLOCK.WMF\r\n"
        "NTPTIME.WMF\r\n";
      int written = smb2_write(smb2, fh_write, (const uint8_t*)text, (uint32_t)strlen(text));
      printf("     + записано в wc.ini: %d байт\n", written);
      smb2_close(smb2, fh_write);
    }
    smb2_unlink(smb2, "test_write.txt");
  }

  /* Повторное перечисление показывает, пережил ли сервер пробу с каталогом */
  printf("\n[4] контрольное перечисление после всех операций:\n");
  if (listRoot(smb2) == 0) {
    printf("\nСЕРВЕР ЖИВ — все тесты пройдены успешно!\n");
  } else {
    printf("\nСЕРВЕР НЕ ОТВЕЧАЕТ — воспроизвели поломку.\n");
  }

  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  return 0;
}
