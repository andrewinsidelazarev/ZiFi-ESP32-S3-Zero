/*
 * Повтор «хвоста Проводника» после успешного чтения файла.
 *
 * На железе копирование вставало не на данных: файл вычитывался целиком за
 * полсекунды, после чего Проводник открывал ВТОРОЙ дескриптор того же файла и
 * спрашивал метаданные — классы 48 (FileNormalizedNameInformation) с крошечным
 * выходным буфером, затем 22 (FileStreamInformation) с буфером 64 КиБ, затем
 * снова 48 уже с настоящим размером и 34 (FileNetworkOpenInformation). После
 * последнего запроса сервер замолкал навсегда.
 *
 * Программа повторяет ровно эту последовательность и печатает, на каком шаге
 * ответ не пришёл.
 *
 * Запуск: smb_tailcheck.exe <хост:порт> <ресурс> <логин> <пароль> <файл>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Диагностический хук прошивки вкомпилирован в libsmb2-dcerpc.c, а живёт в
 * diagnostic_log.cpp, которого в клиенте нет. */
void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b);
void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b) {
  (void)stage;
  (void)a;
  (void)b;
}

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>

/* Первые поля struct smb2fh из libsmb2: нужен только file_id открытого файла,
 * а публичного способа его получить библиотека не даёт. */
struct probe_fh_mirror {
  smb2_command_cb cb;
  void* cb_data;
  smb2_file_id file_id;
  int64_t offset;
  int64_t end_of_file;
};

struct probe_wait {
  int done;
  int status;
};

static void infoCb(struct smb2_context* smb2, int status, void* command_data,
                   void* private_data) {
  struct probe_wait* wait = private_data;
  (void)smb2;
  (void)command_data;
  wait->status = status;
  wait->done = 1;
}

/* Один QUERY_INFO с явными классом и размером выходного буфера. Возвращает 0,
 * если ответ пришёл, и -1, если сервер промолчал дольше отведённого времени. */
static int queryInfo(struct smb2_context* smb2, struct smb2fh* handle,
                     uint8_t infoType, uint8_t infoClass, uint32_t outputLength,
                     int seconds) {
  struct probe_fh_mirror* mirror = (struct probe_fh_mirror*)handle;
  struct smb2_query_info_request req;
  struct smb2_pdu* pdu;
  struct probe_wait wait;
  DWORD started;

  memset(&req, 0, sizeof(req));
  req.info_type = infoType;
  req.file_info_class = infoClass;
  req.output_buffer_length = outputLength;
  memcpy(req.file_id, mirror->file_id, sizeof(req.file_id));

  wait.done = 0;
  wait.status = -1;
  pdu = smb2_cmd_query_info_async(smb2, &req, infoCb, &wait);
  if (pdu == NULL) {
    printf("    класс %u: запрос не собрался: %s\n", (unsigned)infoClass,
           smb2_get_error(smb2));
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);

  started = GetTickCount();
  while (!wait.done) {
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)smb2_get_fd(smb2);
    pfd.events = POLLIN;
    if (smb2_which_events(smb2) & POLLOUT) {
      pfd.events |= POLLOUT;
    }
    pfd.revents = 0;
    if (WSAPoll(&pfd, 1, 500) > 0) {
      if (smb2_service(smb2, pfd.revents) < 0) {
        printf("    класс %u: обрыв обмена: %s\n", (unsigned)infoClass,
               smb2_get_error(smb2));
        return -1;
      }
    }
    if ((DWORD)(GetTickCount() - started) > (DWORD)seconds * 1000) {
      printf("    класс %u (буфер %lu): ОТВЕТА НЕТ — сервер замолчал\n",
             (unsigned)infoClass, (unsigned long)outputLength);
      return -1;
    }
  }
  printf("    класс %u (буфер %lu): ответ, статус 0x%08x\n",
         (unsigned)infoClass, (unsigned long)outputLength,
         (unsigned)wait.status);
  return 0;
}

int main(int argc, char** argv) {
  struct smb2_context* smb2;
  struct smb2fh* first;
  struct smb2fh* second;
  uint8_t buffer[65536];
  uint64_t total = 0;
  int got;
  int failures = 0;
  const char* name;

  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 6) {
    printf("Использование: smb_tailcheck.exe <хост:порт> <ресурс> <логин>"
           " <пароль> <файл>\n");
    return 2;
  }
  name = argv[5];

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

  printf("[1] чтение %s\n", name);
  first = smb2_open(smb2, name, 0 /* O_RDONLY */);
  if (first == NULL) {
    printf("    открыть не удалось: %s\n", smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return 2;
  }
  while ((got = smb2_read(smb2, first, buffer, sizeof(buffer))) > 0) {
    total += (uint64_t)got;
  }
  printf("    прочитано %llu байт\n", (unsigned long long)total);

  printf("[2] второй дескриптор того же файла\n");
  second = smb2_open(smb2, name, 0 /* O_RDONLY */);
  if (second == NULL) {
    printf("    открыть не удалось: %s\n", smb2_get_error(smb2));
    smb2_close(smb2, first);
    smb2_destroy_context(smb2);
    return 2;
  }

  printf("[3] хвост метаданных, как его шлёт Проводник\n");
  /* Порядок и размеры буферов взяты из кольцевого журнала железа. */
  failures += queryInfo(smb2, second, SMB2_0_INFO_FILE, 48, 8, 10) != 0;
  failures += queryInfo(smb2, second, SMB2_0_INFO_FILE, 22, 65536, 10) != 0;
  failures += queryInfo(smb2, second, SMB2_0_INFO_FILE, 48, 252, 10) != 0;
  failures += queryInfo(smb2, second, SMB2_0_INFO_FILE, 34, 56, 10) != 0;

  printf("[4] закрытие обоих дескрипторов\n");
  smb2_close(smb2, second);
  smb2_close(smb2, first);
  printf("    закрыты\n");

  printf("[5] контрольное перечисление корня\n");
  {
    struct smb2dir* dir = smb2_opendir(smb2, "");
    if (dir == NULL) {
      printf("    СЕРВЕР НЕ ОТВЕЧАЕТ: %s\n", smb2_get_error(smb2));
      ++failures;
    } else {
      int count = 0;
      while (smb2_readdir(smb2, dir) != NULL) {
        ++count;
      }
      smb2_closedir(smb2, dir);
      printf("    сервер жив, элементов %d\n", count);
    }
  }

  printf("\nитог: сбоев %d\n", failures);
  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  return failures == 0 ? 0 : 1;
}
