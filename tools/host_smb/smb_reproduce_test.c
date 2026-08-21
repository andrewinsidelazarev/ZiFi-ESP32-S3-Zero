#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b) {
  (void)stage; (void)a; (void)b;
}

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include "libsmb2-private.h"

struct async_read_state {
  uint8_t *buffer;
  uint32_t length;
  uint64_t offset;
  int done;
  int status;
  int corrupted;
  char error[256];
};

/* Публичный API libsmb2 не выдаёт FileId открытого handle. Начало внутренней
 * структуры стабильно и уже используется отдельным smb_tailcheck. */
struct probe_fh_mirror {
  smb2_command_cb cb;
  void *cb_data;
  smb2_file_id file_id;
  int64_t offset;
  int64_t end_of_file;
};

struct query_info_state {
  int done;
  int status;
};

struct create_context_state {
  int done;
  int status;
  int context_copy_ok;
  uint8_t oplock_level;
  smb2_file_id file_id;
  uint32_t create_context_length;
  uint8_t create_context[128];
};

struct internal_info_state {
  int done;
  int status;
  int decoded;
  uint64_t index_number;
};

static uint16_t test_read_le16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t test_read_le32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t test_read_le64(const uint8_t *data) {
  return (uint64_t)test_read_le32(data) |
         ((uint64_t)test_read_le32(data + 4) << 32);
}

static void test_write_le16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
}

static void test_write_le32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
}

static const char *test_directory = "";
static char test_path_buffers[8][512];
static unsigned int test_path_index = 0;

static const char *test_path(const char *name) {
  if (test_directory[0] == '\0') {
    return name;
  }
  char *buffer = test_path_buffers[test_path_index++ % 8];
  snprintf(buffer, 512, "%s/%s", test_directory, name);
  return buffer;
}

static uint8_t test_byte(uint64_t offset) {
  return (uint8_t)(((offset * 37u) ^ (offset >> 8) ^ (offset >> 16)) & 0xffu);
}

static void async_read_cb(struct smb2_context *smb2, int status,
                          void *command_data, void *private_data) {
  struct async_read_state *state = (struct async_read_state*)private_data;
  (void)command_data;
  state->status = status;
  state->corrupted = 0;
  if (status == (int)state->length) {
    for (uint32_t index = 0; index < state->length; ++index) {
      if (state->buffer[index] != test_byte(state->offset + index)) {
        ++state->corrupted;
      }
    }
  }
  snprintf(state->error, sizeof(state->error), "%s",
           smb2 == NULL ? "no context" : smb2_get_error(smb2));
  state->done = 1;
}

static int wait_async_reads(struct smb2_context *smb2,
                            struct async_read_state *states,
                            int count, DWORD timeout_ms) {
  const DWORD started = GetTickCount();
  for (;;) {
    int completed = 0;
    for (int index = 0; index < count; ++index) {
      completed += states[index].done != 0;
    }
    if (completed == count) {
      return 0;
    }
    if ((DWORD)(GetTickCount() - started) >= timeout_ms) {
      return -1;
    }

    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)smb2_get_fd(smb2);
    pfd.events = (SHORT)smb2_which_events(smb2);
    pfd.revents = 0;
    const int polled = WSAPoll(&pfd, 1, 500);
    if (polled == SOCKET_ERROR) {
      return -1;
    }
    if (smb2_service(smb2, polled > 0 ? pfd.revents : 0) < 0) {
      return -1;
    }
  }
}

static int service_for(struct smb2_context *smb2, DWORD duration_ms) {
  const DWORD started = GetTickCount();
  while ((DWORD)(GetTickCount() - started) < duration_ms) {
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)smb2_get_fd(smb2);
    pfd.events = (SHORT)smb2_which_events(smb2);
    pfd.revents = 0;
    const int polled = WSAPoll(&pfd, 1, 50);
    if (polled == SOCKET_ERROR ||
        smb2_service(smb2, polled > 0 ? pfd.revents : 0) < 0) {
      return -1;
    }
  }
  return 0;
}

static int wait_for_done(struct smb2_context *smb2, const int *done,
                         DWORD timeout_ms) {
  const DWORD started = GetTickCount();
  while (!*done) {
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)smb2_get_fd(smb2);
    pfd.events = (SHORT)smb2_which_events(smb2);
    pfd.revents = 0;
    {
      const int polled = WSAPoll(&pfd, 1, 500);
      if (polled == SOCKET_ERROR ||
          smb2_service(smb2, polled > 0 ? pfd.revents : 0) < 0 ||
          (DWORD)(GetTickCount() - started) >= timeout_ms) {
        return -1;
      }
    }
  }
  return 0;
}

static void create_context_cb(struct smb2_context *smb2, int status,
                              void *command_data, void *private_data) {
  struct create_context_state *state =
      (struct create_context_state*)private_data;
  struct smb2_create_reply *reply = (struct smb2_create_reply*)command_data;
  (void)smb2;
  state->status = status;
  state->context_copy_ok = 0;
  if (status == SMB2_STATUS_SUCCESS && reply != NULL) {
    state->oplock_level = reply->oplock_level;
    memcpy(state->file_id, reply->file_id, sizeof(state->file_id));
    state->create_context_length = reply->create_context_length;
    if (reply->create_context_length <= sizeof(state->create_context) &&
        (reply->create_context_length == 0 ||
         reply->create_context != NULL)) {
      if (reply->create_context_length != 0) {
        memcpy(state->create_context, reply->create_context,
               reply->create_context_length);
      }
      state->context_copy_ok = 1;
    }
  }
  state->done = 1;
}

static void internal_info_cb(struct smb2_context *smb2, int status,
                             void *command_data, void *private_data) {
  struct internal_info_state *state =
      (struct internal_info_state*)private_data;
  struct smb2_query_info_reply *reply =
      (struct smb2_query_info_reply*)command_data;
  (void)smb2;
  state->status = status;
  if (status == SMB2_STATUS_SUCCESS && reply != NULL &&
      reply->output_buffer != NULL && reply->output_buffer_length == 8) {
    state->index_number =
        test_read_le64((const uint8_t*)reply->output_buffer);
    state->decoded = 1;
  }
  state->done = 1;
}

static int raw_create_with_windows_contexts(
    struct smb2_context *smb2, const char *path,
    struct create_context_state *state) {
  struct smb2_create_request request;
  struct smb2_pdu *pdu;
  uint8_t contexts[56];

  memset(contexts, 0, sizeof(contexts));
  /* Windows-подобная цепочка: MxAc с нулевым Timestamp, затем пустой QFid. */
  test_write_le32(contexts, 32);
  test_write_le16(contexts + 4, 16);
  test_write_le16(contexts + 6, 4);
  test_write_le16(contexts + 10, 24);
  test_write_le32(contexts + 12, 8);
  memcpy(contexts + 16, "MxAc", 4);

  test_write_le16(contexts + 32 + 4, 16);
  test_write_le16(contexts + 32 + 6, 4);
  memcpy(contexts + 32 + 16, "QFid", 4);

  memset(&request, 0, sizeof(request));
  request.requested_oplock_level = SMB2_OPLOCK_LEVEL_BATCH;
  request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  request.desired_access = SMB2_FILE_READ_DATA | SMB2_FILE_READ_EA |
                           SMB2_FILE_READ_ATTRIBUTES | SMB2_READ_CONTROL |
                           SMB2_SYNCHRONIZE;
  request.file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
  request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE |
                         SMB2_FILE_SHARE_DELETE;
  request.create_disposition = SMB2_FILE_OPEN;
  request.create_options = SMB2_FILE_NON_DIRECTORY_FILE;
  request.name = path;
  request.create_context_length = sizeof(contexts);
  request.create_context = contexts;
  memset(state, 0, sizeof(*state));

  pdu = smb2_cmd_create_async(smb2, &request, create_context_cb, state);
  if (pdu == NULL) {
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  return wait_for_done(smb2, &state->done, 10000);
}

static int query_internal_file_id(struct smb2_context *smb2,
                                  const smb2_file_id file_id,
                                  struct internal_info_state *state) {
  struct smb2_query_info_request request;
  struct smb2_pdu *pdu;
  int old_passthrough = 0;

  memset(&request, 0, sizeof(request));
  request.info_type = SMB2_0_INFO_FILE;
  request.file_info_class = SMB2_FILE_INTERNAL_INFORMATION;
  request.output_buffer_length = 8;
  memcpy(request.file_id, file_id, sizeof(request.file_id));
  memset(state, 0, sizeof(*state));

  /* Эта версия libsmb2 кодирует FileInternalInformation на сервере, но не
   * декодирует его на клиенте. Passthrough даёт тесту ровно 8 wire-байт. */
  smb2_get_passthrough(smb2, &old_passthrough);
  smb2_set_passthrough(smb2, 1);
  pdu = smb2_cmd_query_info_async(smb2, &request, internal_info_cb, state);
  if (pdu == NULL) {
    smb2_set_passthrough(smb2, old_passthrough);
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  {
    const int result = wait_for_done(smb2, &state->done, 10000);
    smb2_set_passthrough(smb2, old_passthrough);
    return result;
  }
}

static int find_directory_file_id(struct smb2_context *smb2,
                                  const char *directory, const char *name,
                                  uint64_t *file_id) {
  struct smb2dir *dir = smb2_opendir(smb2, directory);
  struct smb2dirent *entry;
  if (dir == NULL) {
    return -1;
  }
  while ((entry = smb2_readdir(smb2, dir)) != NULL) {
    if (strcmp(entry->name, name) == 0) {
      *file_id = entry->st.smb2_ino;
      smb2_closedir(smb2, dir);
      return 0;
    }
  }
  smb2_closedir(smb2, dir);
  return -1;
}

static int inspect_create_context_reply(
    const struct create_context_state *state, uint32_t *query_status,
    uint32_t *maximal_access, uint64_t *disk_file_id, uint64_t *volume_id) {
  size_t offset = 0;
  int seen_mxac = 0;
  int seen_qfid = 0;
  int context_count = 0;
  if (!state->context_copy_ok) {
    return -1;
  }
  while (offset < state->create_context_length) {
    const uint8_t *context = state->create_context + offset;
    const size_t remaining = state->create_context_length - offset;
    uint32_t next;
    uint16_t name_offset;
    uint16_t name_length;
    uint16_t data_offset;
    uint32_t data_length;
    size_t context_length;
    if (remaining < 16) {
      return -1;
    }
    next = test_read_le32(context);
    name_offset = test_read_le16(context + 4);
    name_length = test_read_le16(context + 6);
    data_offset = test_read_le16(context + 10);
    data_length = test_read_le32(context + 12);
    context_length = next == 0 ? remaining : next;
    if (context_length < 16 || context_length > remaining ||
        name_offset > context_length || name_length > context_length - name_offset ||
        data_offset > context_length || data_length > context_length - data_offset ||
        (next != 0 && (next & 7u) != 0)) {
      return -1;
    }
    ++context_count;
    if (name_length == 4 && memcmp(context + name_offset, "MxAc", 4) == 0) {
      if (seen_mxac || data_length != 8) {
        return -1;
      }
      *query_status = test_read_le32(context + data_offset);
      *maximal_access = test_read_le32(context + data_offset + 4);
      seen_mxac = 1;
    } else if (name_length == 4 &&
               memcmp(context + name_offset, "QFid", 4) == 0) {
      int reserved_zero = 1;
      size_t index;
      if (seen_qfid || data_length != 32) {
        return -1;
      }
      *disk_file_id = test_read_le64(context + data_offset);
      *volume_id = test_read_le64(context + data_offset + 8);
      for (index = 16; index < 32; ++index) {
        if (context[data_offset + index] != 0) {
          reserved_zero = 0;
        }
      }
      if (!reserved_zero) {
        return -1;
      }
      seen_qfid = 1;
    } else {
      return -1;
    }
    if (next == 0) {
      break;
    }
    offset += next;
  }
  return seen_mxac && seen_qfid && context_count == 2 ? 0 : -1;
}

static void query_info_cb(struct smb2_context *smb2, int status,
                          void *command_data, void *private_data) {
  struct query_info_state *state = (struct query_info_state*)private_data;
  (void)smb2;
  (void)command_data;
  state->status = status;
  state->done = 1;
}

static int query_security_status(struct smb2_context *smb2,
                                 struct smb2fh *handle,
                                 int *result_status) {
  struct probe_fh_mirror *mirror = (struct probe_fh_mirror*)handle;
  struct smb2_query_info_request request;
  struct query_info_state state;
  struct smb2_pdu *pdu;
  const DWORD started = GetTickCount();

  memset(&request, 0, sizeof(request));
  request.info_type = SMB2_0_INFO_SECURITY;
  request.output_buffer_length = 65536;
  request.additional_information = SMB2_ATTRIBUTE_SECURITY_INFORMATION;
  memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
  memset(&state, 0, sizeof(state));

  pdu = smb2_cmd_query_info_async(smb2, &request, query_info_cb, &state);
  if (pdu == NULL) {
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  while (!state.done) {
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)smb2_get_fd(smb2);
    pfd.events = (SHORT)smb2_which_events(smb2);
    pfd.revents = 0;
    const int polled = WSAPoll(&pfd, 1, 500);
    if (polled == SOCKET_ERROR ||
        smb2_service(smb2, polled > 0 ? pfd.revents : 0) < 0 ||
        (DWORD)(GetTickCount() - started) >= 10000) {
      return -1;
    }
  }
  *result_status = state.status;
  return 0;
}

static int query_file_regions_status(struct smb2_context *smb2,
                                     struct smb2fh *handle,
                                     int *result_status) {
  struct probe_fh_mirror *mirror = (struct probe_fh_mirror*)handle;
  struct smb2_ioctl_request request;
  struct query_info_state state;
  struct smb2_pdu *pdu;
  uint8_t input[24];
  const DWORD started = GetTickCount();

  memset(input, 0, sizeof(input));
  /* FILE_REGION_USAGE_VALID_CACHED_DATA. Остальные поля запрашивают весь
   * файл, как реальный Windows CopyFile из аппаратного журнала. */
  input[16] = 1;
  memset(&request, 0, sizeof(request));
  request.ctl_code = FSCTL_QUERY_FILE_REGIONS;
  memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
  request.input_count = sizeof(input);
  request.max_output_response = 64;
  request.flags = SMB2_0_IOCTL_IS_FSCTL;
  request.input = input;
  memset(&state, 0, sizeof(state));

  pdu = smb2_cmd_ioctl_async(smb2, &request, query_info_cb, &state);
  if (pdu == NULL) {
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  while (!state.done) {
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)smb2_get_fd(smb2);
    pfd.events = (SHORT)smb2_which_events(smb2);
    pfd.revents = 0;
    const int polled = WSAPoll(&pfd, 1, 500);
    if (polled == SOCKET_ERROR ||
        smb2_service(smb2, polled > 0 ? pfd.revents : 0) < 0 ||
        (DWORD)(GetTickCount() - started) >= 10000) {
      return -1;
    }
  }
  *result_status = state.status;
  return 0;
}

static struct smb2_context *connect_context(const char *server,
                                            const char *share) {
  struct smb2_context *context = smb2_init_context();
  if (context == NULL) {
    return NULL;
  }
  smb2_set_security_mode(context, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_password(context, "zx");
  if (smb2_connect_share(context, server, share, "zx") != 0) {
    printf("  Secondary connect to %s/%s failed: %s\n",
           server, share, smb2_get_error(context));
    smb2_destroy_context(context);
    return NULL;
  }
  return context;
}

static void make_samba_server_guid(const char *netbios_name,
                                   unsigned char output[16]) {
  size_t index;
  memset(output, 0, 16);
  for (index = 0; index < 15 && netbios_name[index] != '\0'; ++index) {
    unsigned char value = (unsigned char)netbios_name[index];
    if (value >= 'A' && value <= 'Z') {
      value = (unsigned char)(value + ('a' - 'A'));
    }
    output[index] = value;
  }
}

int main(int argc, char **argv) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
    printf("FAIL: WSAStartup failed\n");
    return 1;
  }

  if (argc < 3) {
    printf("Usage: smb_reproduce_test host[:port] share [all|basic|test8|test9|test10|test11|test12|test13|test14|test15|test16|test17] [test-directory]\n");
    return 1;
  }
  const char *server = argv[1];
  const char *share = argv[2];
  if (argc >= 5) {
    test_directory = argv[4];
  }
  int failures = 0;

  struct smb2_context *smb2 = smb2_init_context();
  if (!smb2) {
    printf("FAIL: smb2_init_context failed\n");
    return 1;
  }
  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_password(smb2, "zx");
  
  if (smb2_connect_share(smb2, server, share, "zx") != 0) {
    printf("FAIL: Connect to %s/%s failed: %s\n", server, share, smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return 1;
  }
  printf("SUCCESS: Connected to %s/%s\n", server, share);

  /* Windows-клиент SMB2 требует не меньше четырёх кредитов. При трёх
   * CopyFile выполняет три предварительных READ и больше не отправляет
   * запросы, ожидая четвёртый кредит. */
  {
    const int credit_ok = smb2->credits >= 4;
    printf("NEGOTIATED CREDITS: %d (%s)\n", smb2->credits,
           credit_ok ? "PASS" : "FAIL: Windows requires at least 4");
    failures += !credit_ok;
  }

  /* Samba smbd связывает ServerGuid со стабильным NetBIOS-именем, а SessionId
   * создаёт отдельно и случайно. Windows SMB3 хранит ServerList по имени и
   * разрывает NEGOTIATE, если для уже известного имени внезапно пришёл другой
   * ServerGuid. Проверяем оба независимых контракта на двух соединениях. */
  {
    unsigned char expected_guid[16];
    uint64_t session_id = 0;
    uint64_t peer_session_id = 0;
    struct smb2_context *peer = connect_context(server, share);
    const unsigned char *server_guid =
        (const unsigned char *)smb2_get_server_guid(smb2);
    const unsigned char *peer_guid =
        peer == NULL ? NULL :
        (const unsigned char *)smb2_get_server_guid(peer);
    const int have_session = smb2_get_session_id(smb2, &session_id) == 0;
    const int peer_has_session = peer != NULL &&
        smb2_get_session_id(peer, &peer_session_id) == 0;
    make_samba_server_guid("ZX-Evo", expected_guid);
    const int guid_ok = server_guid != NULL && peer_guid != NULL &&
                        memcmp(server_guid, expected_guid, 16) == 0 &&
                        memcmp(peer_guid, expected_guid, 16) == 0 &&
                        memcmp(server_guid, peer_guid, 16) == 0;
    const int session_ok = have_session && peer_has_session &&
                           session_id != 0 && peer_session_id != 0 &&
                           session_id != 0x1234 &&
                           peer_session_id != 0x1234 &&
                           session_id != peer_session_id;
    printf("SERVER IDENTITY: samba-guid=%s sessions=0x%016llx/0x%016llx (%s)\n",
           guid_ok ? "PASS" : "FAIL", (unsigned long long)session_id,
           (unsigned long long)peer_session_id,
           session_ok ? "PASS" : "FAIL");
    failures += !guid_ok || !session_ok;
    if (peer != NULL) {
      smb2_disconnect_share(peer);
      smb2_destroy_context(peer);
    }
  }

  const char *only_test = argc >= 4 ? argv[3] : NULL;
  if (only_test != NULL && strcmp(only_test, "all") == 0) {
    only_test = NULL;
  }
  if (only_test != NULL) {
    if (strcmp(only_test, "test8") == 0) goto test_8;
    if (strcmp(only_test, "test9") == 0) goto test_9;
    if (strcmp(only_test, "test10") == 0) goto test_10;
    if (strcmp(only_test, "test11") == 0) goto test_11;
    if (strcmp(only_test, "test12") == 0) goto test_12;
    if (strcmp(only_test, "test13") == 0) goto test_13;
    if (strcmp(only_test, "test14") == 0) goto test_14;
    if (strcmp(only_test, "test15") == 0) goto test_15;
    if (strcmp(only_test, "test16") == 0) goto test_16;
    if (strcmp(only_test, "test17") == 0) goto test_17;
  }

  /* TEST 1: Sequential & Multiple Reads check */
  printf("\n--- TEST 1: Sequential & Chunked File Reading ---\n");
  struct smb2fh *fh_w = smb2_open(smb2, test_path("read_test.bin"), O_CREAT | O_WRONLY);
  if (fh_w) {
    uint8_t dummy[16384];
    for (int i = 0; i < 16384; ++i) dummy[i] = (uint8_t)(i & 0xFF);
    smb2_write(smb2, fh_w, dummy, 16384);
    smb2_close(smb2, fh_w);
  }
  struct smb2fh *fh_r = smb2_open(smb2, test_path("read_test.bin"), O_RDONLY);
  int read_ok = 0;
  if (fh_r) {
    uint8_t in_buf[16384] = {0};
    int r = smb2_read(smb2, fh_r, in_buf, 16384);
    smb2_close(smb2, fh_r);
    if (r == 16384) {
      read_ok = 1;
      printf("  Read 16384 bytes successfully!\n");
    } else {
      printf("  Read failed: got %d bytes\n", r);
    }
  }
  printf("RESULT TEST 1: %s\n", read_ok ? "PASS" : "FAIL");
  failures += !read_ok;

  /* TEST 2: explicit ReplaceIfExists rename semantics. */
  printf("\n--- TEST 2: Rename ReplaceIfExists (Notepad Pattern) ---\n");
  struct smb2fh *f1 = smb2_open(smb2, test_path("np_target.txt"),
                                O_CREAT | O_TRUNC | O_WRONLY);
  if (f1) {
    smb2_write(smb2, f1, (const uint8_t*)"ORIGINAL_CONTENT", 16);
    smb2_close(smb2, f1);
  }
  struct smb2fh *f2 = smb2_open(smb2, test_path("np_temp.tmp"),
                                O_CREAT | O_TRUNC | O_WRONLY);
  if (f2) {
    smb2_write(smb2, f2, (const uint8_t*)"NEW_REPLACED_VAL", 16);
    smb2_close(smb2, f2);
  }

  int ren_res = smb2_rename_replace(smb2, test_path("np_temp.tmp"),
                                    test_path("np_target.txt"), 1);
  int test2_ok = ren_res == 0;
  printf("  ReplaceIfExists rename result: %d (error: %s)\n", ren_res,
         smb2_get_error(smb2));
  struct smb2fh *f_chk = smb2_open(smb2, test_path("np_target.txt"), O_RDONLY);
  char buf[32] = {0};
  if (f_chk) {
    int got = smb2_read(smb2, f_chk, (uint8_t*)buf, 16);
    smb2_close(smb2, f_chk);
    printf("  Target file content after replace: \"%s\"\n", buf);
    test2_ok = test2_ok && got == 16 &&
               memcmp(buf, "NEW_REPLACED_VAL", 16) == 0;
  } else {
    test2_ok = 0;
  }
  printf("RESULT TEST 2: %s\n", test2_ok ? "PASS" : "FAIL");
  failures += !test2_ok;

  /* TEST 3: Timestamps check */
  printf("\n--- TEST 3: File Timestamps Check ---\n");
  struct smb2_stat_64 st3;
  int test3_ok = 0;
  if (smb2_stat(smb2, test_path("np_target.txt"), &st3) == 0) {
    printf("  np_target.txt smb2_mtime: %ld\n", (long)st3.smb2_mtime);
    if (st3.smb2_mtime <= 0 || st3.smb2_mtime < 315532800) {
      printf("RESULT TEST 3: BUG CONFIRMED (Timestamp is 0 / year 1601: %ld)\n", (long)st3.smb2_mtime);
    } else {
      test3_ok = 1;
      printf("RESULT TEST 3: PASS (Valid timestamp: %ld)\n", (long)st3.smb2_mtime);
    }
  } else {
    printf("RESULT TEST 3: stat failed\n");
  }
  failures += !test3_ok;

  /* Helper to test arbitrary file size reading in 64KB buffers */
  int test_file_sizes[] = {10000, 65536, 65537, 53248};
  const char* test_file_names[] = {"test_10k.bin", "test_64k.bin", "test_64kp1.bin", "evogram.bin"};
  int num_tests = sizeof(test_file_sizes) / sizeof(test_file_sizes[0]);

  for (int t = 0; t < num_tests; ++t) {
    int expected_size = test_file_sizes[t];
    const char* fname = test_file_names[t];
    printf("\n--- TEST %d: %d bytes reading via 64KB buffer (%s) ---\n", t + 4, expected_size, fname);

    struct smb2fh *fh_w = smb2_open(smb2, test_path(fname), O_CREAT | O_WRONLY);
    if (fh_w) {
      uint8_t *big = (uint8_t*)malloc(expected_size);
      for (int i = 0; i < expected_size; ++i) big[i] = (uint8_t)(i & 0xFF);
      int written = 0;
      while (written < expected_size) {
        int chunk = (expected_size - written > 65536) ? 65536 : (expected_size - written);
        int w = smb2_write(smb2, fh_w, big + written, chunk);
        if (w <= 0) break;
        written += w;
      }
      smb2_close(smb2, fh_w);
      free(big);
    }

    struct smb2fh *fh_r = smb2_open(smb2, test_path(fname), O_RDONLY);
    int total_read = 0;
    int corrupted = 0;
    if (fh_r) {
      uint8_t *buf_64k = (uint8_t*)malloc(65536);
      int iters = 0;
      while (iters++ < 10) {
        int r = smb2_read(smb2, fh_r, buf_64k, 65536);
        printf("  Iteration %d: smb2_read(65536) returned %d\n", iters, r);
        if (r <= 0) break;
        for (int i = 0; i < r; ++i) {
          if (buf_64k[i] != (uint8_t)((total_read + i) & 0xFF)) {
            corrupted++;
          }
        }
        total_read += r;
      }
      smb2_close(smb2, fh_r);
      free(buf_64k);
    }
    printf("  Total read: %d / %d (corrupted: %d)\n", total_read, expected_size, corrupted);
    const int size_ok = total_read == expected_size && corrupted == 0;
    printf("RESULT TEST %d: %s\n", t + 4, size_ok ? "PASS" : "FAIL");
    failures += !size_ok;
  }
  if (only_test != NULL && strcmp(only_test, "basic") == 0) goto done;

test_8:
  /* TEST 8: несколько одновременно ожидающих READ, как у CopyFile. */
  printf("\n--- TEST 8: Three pipelined out-of-order PREAD requests ---\n");
  {
    const uint32_t chunk_size = 65536;
    const uint32_t file_size = chunk_size * 3;
    const uint64_t offsets[3] = {chunk_size, 0, chunk_size * 2};
    struct async_read_state states[3];
    uint8_t *write_buffer = (uint8_t*)malloc(chunk_size);
    struct smb2fh *write_handle = NULL;
    struct smb2fh *read_handle = NULL;
    int test_ok = write_buffer != NULL;

    if (test_ok) {
      write_handle = smb2_open(smb2, test_path("async_read.bin"),
                               O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = write_handle != NULL;
    }
    for (uint32_t offset = 0; test_ok && offset < file_size;
         offset += chunk_size) {
      for (uint32_t index = 0; index < chunk_size; ++index) {
        write_buffer[index] = test_byte((uint64_t)offset + index);
      }
      const int written = smb2_write(smb2, write_handle, write_buffer,
                                     chunk_size);
      if (written != (int)chunk_size) {
        printf("  Prepare failed at offset %lu: wrote %d (%s)\n",
               (unsigned long)offset, written, smb2_get_error(smb2));
        test_ok = 0;
      }
    }
    if (write_handle != NULL) {
      if (smb2_close(smb2, write_handle) != 0) {
        test_ok = 0;
      }
    }
    free(write_buffer);

    if (test_ok) {
      read_handle = smb2_open(smb2, test_path("async_read.bin"), O_RDONLY);
      test_ok = read_handle != NULL;
    }
    memset(states, 0, sizeof(states));
    for (int index = 0; test_ok && index < 3; ++index) {
      states[index].length = chunk_size;
      states[index].offset = offsets[index];
      states[index].status = -1;
      states[index].buffer = (uint8_t*)malloc(chunk_size);
      if (states[index].buffer == NULL ||
          smb2_pread_async(smb2, read_handle, states[index].buffer,
                           chunk_size, states[index].offset, async_read_cb,
                           &states[index]) != 0) {
        printf("  Queue %d failed: %s\n", index, smb2_get_error(smb2));
        test_ok = 0;
      }
    }
    if (test_ok && wait_async_reads(smb2, states, 3, 120000) != 0) {
      printf("  Timeout/service failure: %s\n", smb2_get_error(smb2));
      test_ok = 0;
    }
    for (int index = 0; index < 3; ++index) {
      printf("  PREAD %d offset=%llu status=%d corrupted=%d error=%s\n", index,
             (unsigned long long)states[index].offset, states[index].status,
             states[index].corrupted, states[index].error);
      if (!states[index].done || states[index].status != (int)chunk_size ||
          states[index].corrupted != 0) {
        test_ok = 0;
      }
      free(states[index].buffer);
    }
    if (read_handle != NULL) {
      smb2_close(smb2, read_handle);
    }
    printf("RESULT TEST 8: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }
  if (only_test != NULL) goto done;

test_9:
  /* TEST 9: отключение клиента с ожидающим READ не должно убить чужой READ. */
  printf("\n--- TEST 9: Disconnect pending reader while another READ is active ---\n");
  {
    const uint32_t length = 65536;
    uint8_t *write_buffer = (uint8_t*)malloc(length);
    struct smb2fh *write_handle = NULL;
    struct smb2fh *read_a = NULL;
    struct smb2fh *read_b = NULL;
    struct smb2_context *second = NULL;
    struct async_read_state state_a;
    struct async_read_state state_b;
    int test_ok = write_buffer != NULL;

    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    state_a.length = length;
    state_b.length = length;
    state_a.status = -1;
    state_b.status = -1;
    state_a.buffer = (uint8_t*)malloc(length);
    state_b.buffer = (uint8_t*)malloc(length);
    test_ok = test_ok && state_a.buffer != NULL && state_b.buffer != NULL;

    if (test_ok) {
      for (uint32_t index = 0; index < length; ++index) {
        write_buffer[index] = test_byte(index);
      }
      write_handle = smb2_open(smb2, test_path("disconnect_read.bin"),
                               O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = write_handle != NULL &&
                smb2_write(smb2, write_handle, write_buffer, length) ==
                    (int)length;
    }
    if (write_handle != NULL) {
      test_ok = smb2_close(smb2, write_handle) == 0 && test_ok;
    }
    free(write_buffer);

    if (test_ok) {
      second = connect_context(server, share);
      test_ok = second != NULL;
    }
    if (test_ok) {
      read_a = smb2_open(smb2, test_path("disconnect_read.bin"), O_RDONLY);
      read_b = smb2_open(second, test_path("disconnect_read.bin"), O_RDONLY);
      test_ok = read_a != NULL && read_b != NULL;
    }
    if (test_ok) {
      test_ok = smb2_pread_async(smb2, read_a, state_a.buffer, length, 0,
                                 async_read_cb, &state_a) == 0;
    }
    if (test_ok) {
      test_ok = service_for(smb2, 300) == 0;
    }
    if (test_ok) {
      test_ok = smb2_pread_async(second, read_b, state_b.buffer, length, 0,
                                 async_read_cb, &state_b) == 0;
    }
    if (test_ok) {
      test_ok = service_for(second, 500) == 0;
    }

    /* Закрываем второй TCP-контекст, пока его READ ждёт первый VFS-обмен. */
    if (second != NULL) {
      smb2_destroy_context(second);
      second = NULL;
      read_b = NULL;
    }
    if (test_ok && wait_async_reads(smb2, &state_a, 1, 45000) != 0) {
      printf("  Active reader timed out after peer disconnect: %s\n",
             smb2_get_error(smb2));
      test_ok = 0;
    }
    if (!state_a.done || state_a.status != (int)length ||
        state_a.corrupted != 0) {
      test_ok = 0;
    }
    printf("  Surviving PREAD status=%d corrupted=%d error=%s\n",
           state_a.status, state_a.corrupted, state_a.error);
    if (read_a != NULL) {
      smb2_close(smb2, read_a);
    }
    free(state_a.buffer);
    free(state_b.buffer);
    printf("RESULT TEST 9: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }
  if (only_test != NULL) goto done;

test_10:
  /* TEST 10: активный READ ушедшего клиента должен освободить мост для чужого. */
  printf("\n--- TEST 10: Disconnect active reader while peer READ is pending ---\n");
  {
    const uint32_t length = 65536;
    uint8_t *write_buffer = (uint8_t*)malloc(length);
    struct smb2fh *write_handle = NULL;
    struct smb2fh *read_a = NULL;
    struct smb2fh *read_b = NULL;
    struct smb2_context *second = NULL;
    struct async_read_state state_a;
    struct async_read_state state_b;
    int test_ok = write_buffer != NULL;

    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    state_a.length = length;
    state_b.length = length;
    state_a.status = -1;
    state_b.status = -1;
    state_a.buffer = (uint8_t*)malloc(length);
    state_b.buffer = (uint8_t*)malloc(length);
    test_ok = test_ok && state_a.buffer != NULL && state_b.buffer != NULL;

    /* Новый путь и WRITE сбрасывают кэш предыдущего теста. */
    if (test_ok) {
      for (uint32_t index = 0; index < length; ++index) {
        write_buffer[index] = test_byte(index);
      }
      write_handle = smb2_open(smb2, test_path("disconnect_active.bin"),
                               O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = write_handle != NULL &&
                smb2_write(smb2, write_handle, write_buffer, length) ==
                    (int)length;
    }
    if (write_handle != NULL) {
      test_ok = smb2_close(smb2, write_handle) == 0 && test_ok;
    }
    free(write_buffer);

    if (test_ok) {
      second = connect_context(server, share);
      test_ok = second != NULL;
    }
    if (test_ok) {
      read_a = smb2_open(smb2, test_path("disconnect_active.bin"), O_RDONLY);
      read_b = smb2_open(second, test_path("disconnect_active.bin"), O_RDONLY);
      test_ok = read_a != NULL && read_b != NULL;
    }
    if (test_ok) {
      test_ok = smb2_pread_async(second, read_b, state_b.buffer, length, 0,
                                 async_read_cb, &state_b) == 0;
    }
    if (test_ok) {
      test_ok = service_for(second, 300) == 0;
    }
    if (test_ok) {
      test_ok = smb2_pread_async(smb2, read_a, state_a.buffer, length, 0,
                                 async_read_cb, &state_a) == 0;
    }
    if (test_ok) {
      test_ok = service_for(smb2, 500) == 0;
    }

    /* B владеет core 1; A уже стоит в серверной очереди. */
    if (second != NULL) {
      smb2_destroy_context(second);
      second = NULL;
      read_b = NULL;
    }
    if (test_ok && wait_async_reads(smb2, &state_a, 1, 45000) != 0) {
      printf("  Pending peer timed out after active disconnect: %s\n",
             smb2_get_error(smb2));
      test_ok = 0;
    }
    if (!state_a.done || state_a.status != (int)length ||
        state_a.corrupted != 0) {
      test_ok = 0;
    }
    printf("  Surviving PREAD status=%d corrupted=%d error=%s\n",
           state_a.status, state_a.corrupted, state_a.error);
    if (read_a != NULL) {
      smb2_close(smb2, read_a);
    }
    free(state_a.buffer);
    free(state_b.buffer);
    printf("RESULT TEST 10: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_11:
  /* TEST 11: большой файл, закрытие писателя и чтение в новом SMB-сеансе. */
  printf("\n--- TEST 11: 600001-byte write and fresh-session read-back ---\n");
  {
    const uint32_t chunk_size = 65536;
    const uint32_t file_size = 600001;
    uint8_t *buffer = (uint8_t*)malloc(chunk_size);
    struct smb2_context *writer = NULL;
    struct smb2_context *reader = NULL;
    struct smb2fh *handle = NULL;
    uint32_t total_written = 0;
    uint32_t total_read = 0;
    int corrupted = 0;
    int test_ok = buffer != NULL;

    if (test_ok) {
      writer = connect_context(server, share);
      test_ok = writer != NULL;
    }
    if (test_ok) {
      handle = smb2_open(writer, test_path("roundtrip_600001.bin"),
                         O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = handle != NULL;
    }
    while (test_ok && total_written < file_size) {
      uint32_t count = file_size - total_written;
      if (count > chunk_size) count = chunk_size;
      for (uint32_t index = 0; index < count; ++index) {
        buffer[index] = test_byte((uint64_t)total_written + index);
      }
      uint32_t sent = 0;
      while (test_ok && sent < count) {
        const int written = smb2_write(writer, handle, buffer + sent,
                                       count - sent);
        if (written <= 0) {
          printf("  Write failed at %lu: %d (%s)\n",
                 (unsigned long)(total_written + sent), written,
                 smb2_get_error(writer));
          test_ok = 0;
        } else {
          sent += (uint32_t)written;
        }
      }
      total_written += sent;
      printf("  Written: %lu / %lu\n", (unsigned long)total_written,
             (unsigned long)file_size);
    }
    if (handle != NULL) {
      test_ok = smb2_close(writer, handle) == 0 && test_ok;
      handle = NULL;
    }
    if (writer != NULL) {
      smb2_disconnect_share(writer);
      smb2_destroy_context(writer);
      writer = NULL;
    }

    if (test_ok) {
      reader = connect_context(server, share);
      test_ok = reader != NULL;
    }
    if (test_ok) {
      handle = smb2_open(reader, test_path("roundtrip_600001.bin"), O_RDONLY);
      test_ok = handle != NULL;
      if (!test_ok) {
        printf("  Fresh-session open failed: %s\n", smb2_get_error(reader));
      }
    }
    while (test_ok && total_read < file_size) {
      uint32_t count = file_size - total_read;
      if (count > chunk_size) count = chunk_size;
      const int received = smb2_read(reader, handle, buffer, count);
      if (received <= 0) {
        printf("  Read failed at %lu: %d (%s)\n",
               (unsigned long)total_read, received, smb2_get_error(reader));
        test_ok = 0;
        break;
      }
      for (int index = 0; index < received; ++index) {
        if (buffer[index] != test_byte((uint64_t)total_read + index)) {
          ++corrupted;
        }
      }
      total_read += (uint32_t)received;
      printf("  Read: %lu / %lu, corrupted=%d\n",
             (unsigned long)total_read, (unsigned long)file_size, corrupted);
    }
    if (test_ok) {
      const int eof = smb2_read(reader, handle, buffer, 1);
      if (eof != 0) {
        printf("  EOF check failed: %d (%s)\n", eof, smb2_get_error(reader));
        test_ok = 0;
      }
    }
    if (handle != NULL) {
      test_ok = smb2_close(reader, handle) == 0 && test_ok;
    }
    if (reader != NULL) {
      smb2_disconnect_share(reader);
      smb2_destroy_context(reader);
    }
    free(buffer);
    test_ok = test_ok && total_written == file_size &&
              total_read == file_size && corrupted == 0;
    printf("  Total written=%lu read=%lu corrupted=%d\n",
           (unsigned long)total_written, (unsigned long)total_read, corrupted);
    printf("RESULT TEST 11: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_12:
  /* TEST 12: Windows FlushFileBuffers pattern before CLOSE. */
  printf("\n--- TEST 12: 600001-byte write, explicit FLUSH, fresh read-back ---\n");
  {
    const uint32_t chunk_size = 65536;
    const uint32_t file_size = 600001;
    uint8_t *buffer = (uint8_t*)malloc(chunk_size);
    struct smb2_context *writer = NULL;
    struct smb2_context *reader = NULL;
    struct smb2fh *handle = NULL;
    uint32_t total_written = 0;
    uint32_t total_read = 0;
    int corrupted = 0;
    int flush_result = -1;
    int test_ok = buffer != NULL;

    if (test_ok) {
      writer = connect_context(server, share);
      test_ok = writer != NULL;
    }
    if (test_ok) {
      handle = smb2_open(writer, test_path("flush_600001.bin"),
                         O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = handle != NULL;
    }
    while (test_ok && total_written < file_size) {
      uint32_t count = file_size - total_written;
      if (count > chunk_size) count = chunk_size;
      for (uint32_t index = 0; index < count; ++index) {
        buffer[index] = test_byte((uint64_t)total_written + index);
      }
      const int written = smb2_write(writer, handle, buffer, count);
      if (written <= 0) {
        printf("  Write failed at %lu: %d (%s)\n",
               (unsigned long)total_written, written, smb2_get_error(writer));
        test_ok = 0;
      } else {
        total_written += (uint32_t)written;
      }
    }
    if (test_ok) {
      const DWORD flush_started = GetTickCount();
      flush_result = smb2_fsync(writer, handle);
      printf("  FLUSH result=%d elapsed=%lu ms error=%s\n", flush_result,
             (unsigned long)(GetTickCount() - flush_started),
             smb2_get_error(writer));
      test_ok = flush_result == 0;
    }
    if (handle != NULL) {
      const int close_result = smb2_close(writer, handle);
      printf("  CLOSE result=%d error=%s\n", close_result,
             smb2_get_error(writer));
      test_ok = close_result == 0 && test_ok;
      handle = NULL;
    }
    if (writer != NULL) {
      smb2_disconnect_share(writer);
      smb2_destroy_context(writer);
    }

    if (test_ok) {
      reader = connect_context(server, share);
      test_ok = reader != NULL;
    }
    if (test_ok) {
      handle = smb2_open(reader, test_path("flush_600001.bin"), O_RDONLY);
      test_ok = handle != NULL;
      if (!test_ok) {
        printf("  Fresh-session open after FLUSH failed: %s\n",
               smb2_get_error(reader));
      }
    }
    while (test_ok && total_read < file_size) {
      uint32_t count = file_size - total_read;
      if (count > chunk_size) count = chunk_size;
      const int received = smb2_read(reader, handle, buffer, count);
      if (received <= 0) {
        printf("  Read failed at %lu: %d (%s)\n",
               (unsigned long)total_read, received, smb2_get_error(reader));
        test_ok = 0;
        break;
      }
      for (int index = 0; index < received; ++index) {
        if (buffer[index] != test_byte((uint64_t)total_read + index)) {
          ++corrupted;
        }
      }
      total_read += (uint32_t)received;
    }
    if (handle != NULL) {
      test_ok = smb2_close(reader, handle) == 0 && test_ok;
    }
    if (reader != NULL) {
      smb2_disconnect_share(reader);
      smb2_destroy_context(reader);
    }
    free(buffer);
    test_ok = test_ok && total_written == file_size &&
              total_read == file_size && corrupted == 0;
    printf("  Total written=%lu read=%lu corrupted=%d\n",
           (unsigned long)total_written, (unsigned long)total_read, corrupted);
    printf("RESULT TEST 12: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_13:
  /* TEST 13: CopyFile pattern: reserve EOF, second handle, tail-first writes. */
  printf("\n--- TEST 13: CopyFile reserved EOF and tail-first writes ---\n");
  {
    const uint32_t chunk_size = 65536;
    const uint32_t file_size = 600001;
    const uint32_t offsets[] = {524288, 0, 65536, 131072, 196608,
                                262144, 327680, 393216, 458752, 589824};
    uint8_t *buffer = (uint8_t*)malloc(chunk_size);
    struct smb2_context *writer = NULL;
    struct smb2_context *reader = NULL;
    struct smb2fh *reserve_handle = NULL;
    struct smb2fh *write_handle = NULL;
    struct smb2fh *read_handle = NULL;
    uint32_t total_written = 0;
    uint32_t total_read = 0;
    int corrupted = 0;
    int test_ok = buffer != NULL;

    if (test_ok) {
      writer = connect_context(server, share);
      test_ok = writer != NULL;
    }
    if (test_ok) {
      reserve_handle = smb2_open(writer, test_path("copyfile_600001.bin"),
                                 O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = reserve_handle != NULL;
    }
    if (test_ok) {
      const int reserve_result =
          smb2_ftruncate(writer, reserve_handle, file_size);
      printf("  Reserve EOF result=%d error=%s\n", reserve_result,
             smb2_get_error(writer));
      test_ok = reserve_result == 0;
    }
    if (test_ok) {
      write_handle = smb2_open(writer, test_path("copyfile_600001.bin"),
                               O_WRONLY);
      test_ok = write_handle != NULL;
    }
    for (size_t item = 0;
         test_ok && item < sizeof(offsets) / sizeof(offsets[0]); ++item) {
      const uint32_t offset = offsets[item];
      uint32_t count = file_size - offset;
      if (count > chunk_size) count = chunk_size;
      for (uint32_t index = 0; index < count; ++index) {
        buffer[index] = test_byte((uint64_t)offset + index);
      }
      const int written =
          smb2_pwrite(writer, write_handle, buffer, count, offset);
      printf("  PWRITE offset=%lu count=%lu result=%d error=%s\n",
             (unsigned long)offset, (unsigned long)count, written,
             smb2_get_error(writer));
      test_ok = written == (int)count;
      if (test_ok) total_written += count;
    }
    if (write_handle != NULL) {
      const int close_result = smb2_close(writer, write_handle);
      printf("  Data CLOSE result=%d error=%s\n", close_result,
             smb2_get_error(writer));
      test_ok = close_result == 0 && test_ok;
    }
    if (reserve_handle != NULL) {
      const int close_result = smb2_close(writer, reserve_handle);
      printf("  Reserve CLOSE result=%d error=%s\n", close_result,
             smb2_get_error(writer));
      test_ok = close_result == 0 && test_ok;
    }
    if (writer != NULL) {
      smb2_disconnect_share(writer);
      smb2_destroy_context(writer);
    }

    if (test_ok) {
      reader = connect_context(server, share);
      test_ok = reader != NULL;
    }
    if (test_ok) {
      read_handle = smb2_open(reader, test_path("copyfile_600001.bin"),
                              O_RDONLY);
      test_ok = read_handle != NULL;
      if (!test_ok) {
        printf("  Fresh-session CopyFile open failed: %s\n",
               smb2_get_error(reader));
      }
    }
    while (test_ok && total_read < file_size) {
      uint32_t count = file_size - total_read;
      if (count > chunk_size) count = chunk_size;
      const int received = smb2_read(reader, read_handle, buffer, count);
      if (received <= 0) {
        printf("  Read failed at %lu: %d (%s)\n",
               (unsigned long)total_read, received, smb2_get_error(reader));
        test_ok = 0;
        break;
      }
      for (int index = 0; index < received; ++index) {
        if (buffer[index] != test_byte((uint64_t)total_read + index)) {
          ++corrupted;
        }
      }
      total_read += (uint32_t)received;
    }
    if (read_handle != NULL) {
      test_ok = smb2_close(reader, read_handle) == 0 && test_ok;
    }
    if (reader != NULL) {
      smb2_disconnect_share(reader);
      smb2_destroy_context(reader);
    }
    free(buffer);
    test_ok = test_ok && total_written == file_size &&
              total_read == file_size && corrupted == 0;
    printf("  Total written=%lu read=%lu corrupted=%d\n",
           (unsigned long)total_written, (unsigned long)total_read, corrupted);
    printf("RESULT TEST 13: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_14:
  /* TEST 14: неудачный overwrite открытого reader не должен менять его EOF. */
  printf("\n--- TEST 14: Conflicting overwrite preserves active reader ---\n");
  {
    const uint32_t original_size = 65536;
    const uint32_t replacement_size = 12345;
    uint8_t *buffer = (uint8_t*)malloc(original_size);
    struct smb2_context *reader = NULL;
    struct smb2fh *reader_handle = NULL;
    struct smb2fh *writer_handle = NULL;
    int test_ok = buffer != NULL;
    int corrupted = 0;

    if (test_ok) {
      for (uint32_t index = 0; index < original_size; ++index) {
        buffer[index] = test_byte(index);
      }
      writer_handle = smb2_open(
          smb2, test_path("overwrite_while_reading.bin"),
          O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = writer_handle != NULL &&
                smb2_write(smb2, writer_handle, buffer, original_size) ==
                    (int)original_size;
    }
    if (writer_handle != NULL) {
      test_ok = smb2_close(smb2, writer_handle) == 0 && test_ok;
      writer_handle = NULL;
    }

    if (test_ok) {
      reader = connect_context(server, share);
      test_ok = reader != NULL;
    }
    if (test_ok) {
      reader_handle = smb2_open(
          reader, test_path("overwrite_while_reading.bin"), O_RDONLY);
      test_ok = reader_handle != NULL;
    }
    if (test_ok) {
      memset(buffer, 0, original_size);
      const int received = smb2_pread(reader, reader_handle, buffer,
                                      original_size, 0);
      for (int index = 0; index < received; ++index) {
        if (buffer[index] != test_byte((uint32_t)index)) {
          ++corrupted;
        }
      }
      test_ok = received == (int)original_size && corrupted == 0;
      printf("  Initial reader result=%d corrupted=%d error=%s\n",
             received, corrupted, smb2_get_error(reader));
    }

    if (test_ok) {
      writer_handle = smb2_open(
          smb2, test_path("overwrite_while_reading.bin"),
          O_CREAT | O_TRUNC | O_WRONLY);
      const int rejected = writer_handle == NULL;
      printf("  Conflicting overwrite rejected=%d error=%s\n", rejected,
             smb2_get_error(smb2));
      test_ok = rejected;
      if (writer_handle != NULL) {
        smb2_close(smb2, writer_handle);
        writer_handle = NULL;
      }
    }

    if (test_ok) {
      memset(buffer, 0, original_size);
      const int received = smb2_pread(reader, reader_handle, buffer,
                                      original_size, 0);
      corrupted = 0;
      for (int index = 0; index < received; ++index) {
        if (buffer[index] != test_byte((uint32_t)index)) {
          ++corrupted;
        }
      }
      test_ok = received == (int)original_size && corrupted == 0;
      printf("  Reader after reject=%d corrupted=%d error=%s\n",
             received, corrupted, smb2_get_error(reader));
    }

    if (reader_handle != NULL) {
      test_ok = smb2_close(reader, reader_handle) == 0 && test_ok;
      reader_handle = NULL;
    }

    if (test_ok) {
      for (uint32_t index = 0; index < replacement_size; ++index) {
        buffer[index] = test_byte(index);
      }
      writer_handle = smb2_open(
          smb2, test_path("overwrite_while_reading.bin"),
          O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = writer_handle != NULL &&
                smb2_write(smb2, writer_handle, buffer, replacement_size) ==
                    (int)replacement_size;
    }
    if (writer_handle != NULL) {
      test_ok = smb2_close(smb2, writer_handle) == 0 && test_ok;
      writer_handle = NULL;
    }

    if (test_ok) {
      reader_handle = smb2_open(
          reader, test_path("overwrite_while_reading.bin"), O_RDONLY);
      test_ok = reader_handle != NULL;
    }
    if (test_ok) {
      memset(buffer, 0, original_size);
      const int received = smb2_read(reader, reader_handle, buffer,
                                     original_size);
      const int eof = received > 0
                          ? smb2_read(reader, reader_handle, buffer, 1)
                          : -1;
      corrupted = 0;
      for (int index = 0; index < received; ++index) {
        if (buffer[index] != test_byte((uint32_t)index)) {
          ++corrupted;
        }
      }
      test_ok = received == (int)replacement_size && eof == 0 &&
                corrupted == 0;
      printf("  Replacement read=%d eof=%d corrupted=%d error=%s\n",
             received, eof, corrupted, smb2_get_error(reader));
    }
    if (reader_handle != NULL) {
      test_ok = smb2_close(reader, reader_handle) == 0 && test_ok;
    }
    if (reader != NULL) {
      smb2_disconnect_share(reader);
      smb2_destroy_context(reader);
    }
    free(buffer);
    printf("RESULT TEST 14: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_15:
  /* TEST 15: FAT не имеет ACL. Успешный ответ с фиктивным security descriptor
   * DACL_PROTECTED без DACL ломает Windows CopyFile. Правильный ответ для
   * неподдерживаемого ATTRIBUTE_SECURITY_INFORMATION — STATUS_NOT_SUPPORTED. */
  printf("\n--- TEST 15: Unsupported security info is an SMB error ---\n");
  {
    struct smb2fh *handle = NULL;
    struct smb2fh *writer = smb2_open(
        smb2, test_path("security_query.bin"), O_CREAT | O_TRUNC | O_WRONLY);
    int test_ok = writer != NULL;
    int query_status = 0;
    const uint8_t byte = 0x5a;
    if (test_ok) {
      test_ok = smb2_write(smb2, writer, (void*)&byte, 1) == 1;
      test_ok = smb2_close(smb2, writer) == 0 && test_ok;
      writer = NULL;
    }
    if (test_ok) {
      handle = smb2_open(smb2, test_path("security_query.bin"), O_RDONLY);
      test_ok = handle != NULL;
    }
    if (test_ok) {
      test_ok = query_security_status(smb2, handle, &query_status) == 0 &&
                (uint32_t)query_status == SMB2_STATUS_NOT_SUPPORTED;
      printf("  Security QUERY_INFO status=0x%08x expected=0x%08x\n",
             (unsigned)query_status, (unsigned)SMB2_STATUS_NOT_SUPPORTED);
    }
    if (handle != NULL) {
      test_ok = smb2_close(smb2, handle) == 0 && test_ok;
    }
    printf("RESULT TEST 15: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_16:
  /* TEST 16: FAT не реализует карту valid-data regions. По MS-FSA
   * FSCTL_QUERY_FILE_REGIONS обязан завершаться INVALID_DEVICE_REQUEST, чтобы
   * Windows CopyFile перешёл от оптимизации регионов к обычному чтению. */
  printf("\n--- TEST 16: Unsupported file regions use filesystem status ---\n");
  {
    struct smb2fh *handle = NULL;
    struct smb2fh *writer = smb2_open(
        smb2, test_path("file_regions.bin"), O_CREAT | O_TRUNC | O_WRONLY);
    int test_ok = writer != NULL;
    int ioctl_status = 0;
    const uint8_t byte = 0x5a;
    if (test_ok) {
      test_ok = smb2_write(smb2, writer, &byte, 1) == 1;
      test_ok = smb2_close(smb2, writer) == 0 && test_ok;
      writer = NULL;
    }
    if (test_ok) {
      handle = smb2_open(smb2, test_path("file_regions.bin"), O_RDONLY);
      test_ok = handle != NULL;
    }
    if (test_ok) {
      test_ok = query_file_regions_status(smb2, handle, &ioctl_status) == 0 &&
                (uint32_t)ioctl_status == SMB2_STATUS_INVALID_DEVICE_REQUEST;
      printf("  QUERY_FILE_REGIONS status=0x%08x expected=0x%08x\n",
             (unsigned)ioctl_status,
             (unsigned)SMB2_STATUS_INVALID_DEVICE_REQUEST);
    }
    if (handle != NULL) {
      test_ok = smb2_close(smb2, handle) == 0 && test_ok;
    }
    if (writer != NULL) {
      smb2_close(smb2, writer);
    }
    printf("RESULT TEST 16: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_17:
  /* TEST 17: точный Windows CREATE с MxAc/QFid. Проверяется не наличие кода,
   * а wire-ответ и единый идентификатор в каталоге, QFid и InternalInfo. */
  printf("\n--- TEST 17: Windows MxAc/QFid CREATE contexts and FileId identity ---\n");
  {
    const char *file_name = "create_context_identity.bin";
    const char *path = test_path(file_name);
    const char *directory = test_directory[0] == '\0' ? "" : test_directory;
    const uint8_t byte = 0x5a;
    struct smb2fh *writer = smb2_open(
        smb2, path, O_CREAT | O_TRUNC | O_WRONLY);
    struct create_context_state create_state;
    struct internal_info_state internal_state;
    struct smb2fh *raw_handle = NULL;
    uint64_t directory_file_id = 0;
    uint64_t disk_file_id = 0;
    uint64_t volume_id = 0;
    uint32_t query_status = 0xffffffffu;
    uint32_t maximal_access = 0;
    int test_ok = writer != NULL;

    memset(&create_state, 0, sizeof(create_state));
    memset(&internal_state, 0, sizeof(internal_state));
    create_state.status = -1;
    internal_state.status = -1;

    if (test_ok) {
      test_ok = smb2_write(smb2, writer, (void*)&byte, 1) == 1;
    }
    if (writer != NULL) {
      test_ok = smb2_close(smb2, writer) == 0 && test_ok;
      writer = NULL;
    }
    if (test_ok) {
      test_ok = find_directory_file_id(smb2, directory, file_name,
                                       &directory_file_id) == 0;
    }
    if (test_ok) {
      test_ok = raw_create_with_windows_contexts(
                    smb2, path, &create_state) == 0 &&
                (uint32_t)create_state.status == SMB2_STATUS_SUCCESS &&
                create_state.oplock_level == SMB2_OPLOCK_LEVEL_NONE;
    }
    if (test_ok) {
      test_ok = inspect_create_context_reply(
                    &create_state, &query_status, &maximal_access,
                    &disk_file_id, &volume_id) == 0;
    }
    if (test_ok) {
      test_ok = query_internal_file_id(
                    smb2, create_state.file_id, &internal_state) == 0 &&
                (uint32_t)internal_state.status == SMB2_STATUS_SUCCESS &&
                internal_state.decoded;
    }
    if ((uint32_t)create_state.status == SMB2_STATUS_SUCCESS) {
      raw_handle = smb2_fh_from_file_id(smb2, &create_state.file_id);
      if (raw_handle == NULL || smb2_close(smb2, raw_handle) != 0) {
        test_ok = 0;
      }
    }
    test_ok = test_ok && query_status == SMB2_STATUS_SUCCESS &&
              maximal_access == 0x001F019Fu && volume_id != 0 &&
              directory_file_id != 0 &&
              directory_file_id == disk_file_id &&
              disk_file_id == internal_state.index_number;
    printf("  CREATE status=0x%08x contexts=%lu oplock=0x%02x MxAc=0x%08x/0x%08x\n",
           (unsigned)create_state.status,
           (unsigned long)create_state.create_context_length,
           (unsigned)create_state.oplock_level, (unsigned)query_status,
           (unsigned)maximal_access);
    printf("  FileId directory=0x%016llx QFid=0x%016llx internal=0x%016llx volume=0x%016llx\n",
           (unsigned long long)directory_file_id,
           (unsigned long long)disk_file_id,
           (unsigned long long)internal_state.index_number,
           (unsigned long long)volume_id);
    printf("RESULT TEST 17: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

done:
  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  return failures == 0 ? 0 : 1;
}
