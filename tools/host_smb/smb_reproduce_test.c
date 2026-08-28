#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b) {
  (void)stage; (void)a; (void)b;
}

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-server.h>
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

struct raw_async_read_state {
  uint8_t *buffer;
  uint32_t expected_length;
  uint64_t offset;
  int *completed_total;
  int *event_counter;
  int completed;
  int pending_count;
  int final_count;
  int pending_order;
  int final_order;
  int final_status;
  int corrupted;
  uint16_t pending_command;
  uint16_t final_command;
  uint16_t pending_credit;
  uint16_t final_credit;
  uint32_t pending_flags;
  uint32_t final_flags;
  uint32_t pending_next_command;
  uint32_t final_next_command;
  uint64_t pending_message_id;
  uint64_t final_message_id;
  uint64_t pending_async_id;
  uint64_t final_async_id;
};

struct raw_change_notify_state {
  int *completed_total;
  int completed;
  int pending_count;
  int final_count;
  int final_status;
  int payload_valid;
  uint16_t pending_command;
  uint16_t final_command;
  uint16_t pending_credit;
  uint16_t final_credit;
  uint32_t pending_flags;
  uint32_t final_flags;
  uint64_t pending_message_id;
  uint64_t final_message_id;
  uint64_t pending_async_id;
  uint64_t final_async_id;
  int entry_count;
  uint32_t action;
  uint32_t second_action;
  char name[256];
  char second_name[256];
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

struct allocation_query_state {
  int done;
  int status;
  uint8_t information_class;
  uint64_t allocation_size;
  uint64_t end_of_file;
};

struct windows_compound_state {
  int completed;
  uint32_t request_next_command[3];
  uint32_t request_flags[3];
  int status[3];
  uint32_t next_command[3];
  uint32_t flags[3];
  uint64_t message_id[3];
  uint8_t oplock_level;
  smb2_file_id file_id;
  int context_copy_ok;
  uint32_t create_context_length;
  uint8_t create_context[128];
  int directory_payload_valid[2];
  int directory_padding_zero[2];
  int directory_entry_count[2];
  uint32_t directory_output_length[2];
  uint32_t directory_first_index[2];
  char directory_first_name[2][256];
};

struct raw_directory_state {
  int *completed_total;
  int *event_counter;
  int done;
  int final_order;
  int status;
  int payload_valid;
  int padding_zero;
  int entry_count;
  uint32_t output_length;
  uint32_t first_index;
  char first_name[256];
};

struct internal_id_state {
  int done;
  int status;
  int decoded;
  uint64_t index_number;
};

struct raw_command_state {
  int done;
  int status;
  uint32_t tree_id;
};

struct raw_create_state {
  int done;
  int status;
  smb2_file_id file_id;
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

static void raw_async_read_cb(struct smb2_context *smb2, int status,
                              void *command_data, void *private_data) {
  struct raw_async_read_state *state =
      (struct raw_async_read_state*)private_data;
  if ((uint32_t)status == SMB2_STATUS_PENDING) {
    ++state->pending_count;
    state->pending_order = ++*state->event_counter;
    state->pending_command = smb2->hdr.command;
    state->pending_credit = smb2->hdr.credit_request_response;
    state->pending_flags = smb2->hdr.flags;
    state->pending_next_command = smb2->hdr.next_command;
    state->pending_message_id = smb2->hdr.message_id;
    state->pending_async_id = smb2->hdr.async.async_id;
    return;
  }

  ++state->final_count;
  state->final_order = ++*state->event_counter;
  state->final_status = status;
  state->final_command = smb2->hdr.command;
  state->final_credit = smb2->hdr.credit_request_response;
  state->final_flags = smb2->hdr.flags;
  state->final_next_command = smb2->hdr.next_command;
  state->final_message_id = smb2->hdr.message_id;
  state->final_async_id = smb2->hdr.async.async_id;
  state->corrupted = 0;
  if ((uint32_t)status == SMB2_STATUS_SUCCESS && command_data != NULL) {
    const struct smb2_read_reply *reply =
        (const struct smb2_read_reply*)command_data;
    if (reply->data == NULL || reply->data_length != state->expected_length) {
      state->corrupted = -1;
    } else {
      uint32_t index;
      for (index = 0; index < reply->data_length; ++index) {
        if (reply->data[index] != test_byte(state->offset + index)) {
          ++state->corrupted;
        }
      }
    }
  } else {
    state->corrupted = -1;
  }
  if (!state->completed) {
    state->completed = 1;
    ++*state->completed_total;
  }
}

static void raw_change_notify_cb(struct smb2_context *smb2, int status,
                                 void *command_data, void *private_data) {
  struct raw_change_notify_state *state =
      (struct raw_change_notify_state*)private_data;
  if ((uint32_t)status == SMB2_STATUS_PENDING) {
    ++state->pending_count;
    state->pending_command = smb2->hdr.command;
    state->pending_credit = smb2->hdr.credit_request_response;
    state->pending_flags = smb2->hdr.flags;
    state->pending_message_id = smb2->hdr.message_id;
    state->pending_async_id = smb2->hdr.async.async_id;
    return;
  }

  ++state->final_count;
  state->final_status = status;
  state->final_command = smb2->hdr.command;
  state->final_credit = smb2->hdr.credit_request_response;
  state->final_flags = smb2->hdr.flags;
  state->final_message_id = smb2->hdr.message_id;
  state->final_async_id = smb2->hdr.async.async_id;
  if ((uint32_t)status == SMB2_STATUS_SUCCESS && command_data != NULL) {
    const struct smb2_change_notify_reply *reply =
        (const struct smb2_change_notify_reply*)command_data;
    if (reply->output != NULL && reply->output_buffer_length >= 12) {
      const uint32_t next = test_read_le32(reply->output);
      const uint32_t name_length = test_read_le32(reply->output + 8);
      if ((name_length & 1u) == 0 &&
          name_length <= reply->output_buffer_length - 12 &&
          name_length / 2 < sizeof(state->name)) {
        uint32_t index;
        state->action = test_read_le32(reply->output + 4);
        for (index = 0; index < name_length / 2; ++index) {
          const uint16_t value = test_read_le16(reply->output + 12 + index * 2);
          state->name[index] = value <= 0x7f ? (char)value : '?';
        }
        state->name[name_length / 2] = '\0';
        state->entry_count = 1;
        if (next == 0) {
          state->payload_valid = 1;
        } else if ((next & 3u) == 0 && next >= 12 + name_length &&
                   next + 12 <= reply->output_buffer_length) {
          const uint8_t *second = reply->output + next;
          const uint32_t second_next = test_read_le32(second);
          const uint32_t second_length = test_read_le32(second + 8);
          if (second_next == 0 && (second_length & 1u) == 0 &&
              second_length <= reply->output_buffer_length - next - 12 &&
              second_length / 2 < sizeof(state->second_name)) {
            state->second_action = test_read_le32(second + 4);
            for (index = 0; index < second_length / 2; ++index) {
              const uint16_t value =
                  test_read_le16(second + 12 + index * 2);
              state->second_name[index] = value <= 0x7f ? (char)value : '?';
            }
            state->second_name[second_length / 2] = '\0';
            state->entry_count = 2;
            state->payload_valid = 1;
          }
        }
      }
    }
  }
  if (!state->completed) {
    state->completed = 1;
    ++*state->completed_total;
  }
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

static int wait_for_count(struct smb2_context *smb2, const int *completed,
                          int expected, DWORD timeout_ms) {
  const DWORD started = GetTickCount();
  while (*completed < expected) {
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

static void raw_command_cb(struct smb2_context *smb2, int status,
                           void *command_data, void *private_data) {
  struct raw_command_state *state =
      (struct raw_command_state*)private_data;
  (void)command_data;
  state->status = status;
  state->tree_id = smb2->hdr.sync.tree_id;
  state->done = 1;
}

static int wait_raw_command(struct smb2_context *smb2,
                            struct raw_command_state *state) {
  const DWORD started = GetTickCount();
  while (!state->done) {
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)smb2_get_fd(smb2);
    pfd.events = (SHORT)smb2_which_events(smb2);
    pfd.revents = 0;
    {
      const int polled = WSAPoll(&pfd, 1, 500);
      if (polled == SOCKET_ERROR ||
          smb2_service(smb2, polled > 0 ? pfd.revents : 0) < 0 ||
          (DWORD)(GetTickCount() - started) >= 10000) {
        return -1;
      }
    }
  }
  return 0;
}

static void raw_create_cb(struct smb2_context *smb2, int status,
                          void *command_data, void *private_data) {
  struct raw_create_state *state =
      (struct raw_create_state*)private_data;
  (void)smb2;
  state->status = status;
  if ((uint32_t)status == SMB2_STATUS_SUCCESS && command_data != NULL) {
    const struct smb2_create_reply *reply =
        (const struct smb2_create_reply*)command_data;
    memcpy(state->file_id, reply->file_id, sizeof(state->file_id));
  }
  state->done = 1;
}

/* Точный CREATE каталога из захвата Проводника: DesiredAccess=0x00100081,
 * FileAttributes=NORMAL, ShareAccess=READ|WRITE, FILE_CREATE и
 * DIRECTORY_FILE|SYNCHRONOUS_IO_NONALERT|OPEN_REPARSE_POINT. */
static int raw_windows_create_directory(struct smb2_context *smb2,
                                        const char *path,
                                        struct raw_create_state *state) {
  struct smb2_create_request request;
  struct smb2_pdu *pdu;
  int old_passthrough = 0;

  memset(state, 0, sizeof(*state));
  state->status = -1;
  memset(&request, 0, sizeof(request));
  request.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
  request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  request.desired_access = SMB2_FILE_READ_DATA |
                           SMB2_FILE_READ_ATTRIBUTES |
                           SMB2_SYNCHRONIZE;
  request.file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
  request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE;
  request.create_disposition = SMB2_FILE_CREATE;
  request.create_options = SMB2_FILE_DIRECTORY_FILE |
                           SMB2_FILE_SYNCHRONOUS_IO_NONALERT |
                           SMB2_FILE_OPEN_REPARSE_POINT;
  request.name = path;

  smb2_get_passthrough(smb2, &old_passthrough);
  smb2_set_passthrough(smb2, 1);
  pdu = smb2_cmd_create_async(smb2, &request, raw_create_cb, state);
  if (pdu == NULL) {
    smb2_set_passthrough(smb2, old_passthrough);
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  {
    const int result = wait_for_count(smb2, &state->done, 1, 10000);
    smb2_set_passthrough(smb2, old_passthrough);
    return result;
  }
}

static int raw_set_basic_info(struct smb2_context *smb2, struct smb2fh *fh) {
  struct probe_fh_mirror *mirror = (struct probe_fh_mirror*)fh;
  struct smb2_file_basic_info info;
  struct smb2_set_info_request request;
  struct raw_command_state state;
  struct smb2_pdu *pdu;

  memset(&info, 0, sizeof(info));
  info.last_write_time.tv_sec = (time_t)1700000000;
  info.file_attributes = SMB2_FILE_ATTRIBUTE_ARCHIVE;
  memset(&request, 0, sizeof(request));
  request.info_type = SMB2_0_INFO_FILE;
  request.file_info_class = SMB2_FILE_BASIC_INFORMATION;
  request.input_data = &info;
  memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
  memset(&state, 0, sizeof(state));
  state.status = -1;

  pdu = smb2_cmd_set_info_async(smb2, &request, raw_command_cb, &state);
  if (pdu == NULL) {
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  return wait_raw_command(smb2, &state) == 0 &&
                 (uint32_t)state.status == SMB2_STATUS_SUCCESS
             ? 0
             : -1;
}

static int raw_tree_disconnect(struct smb2_context *smb2,
                               uint32_t *disconnected_tree_id) {
  struct raw_command_state state;
  struct smb2_pdu *pdu;
  memset(&state, 0, sizeof(state));
  state.status = -1;
  pdu = smb2_cmd_tree_disconnect_async(smb2, raw_command_cb, &state);
  if (pdu == NULL) {
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  if (wait_raw_command(smb2, &state) != 0 ||
      (uint32_t)state.status != SMB2_STATUS_SUCCESS) {
    return -1;
  }
  *disconnected_tree_id = state.tree_id;
  return 0;
}

static int raw_tree_connect(struct smb2_context *smb2, const char *share,
                            uint32_t *connected_tree_id) {
  struct raw_command_state state;
  struct smb2_tree_connect_request request;
  struct smb2_pdu *pdu;
  char unc[128];
  uint16_t path[128];
  size_t length;
  size_t index;

  snprintf(unc, sizeof(unc), "\\\\ZX-Evo\\%s", share);
  length = strlen(unc);
  if (length >= sizeof(path) / sizeof(path[0])) {
    return -1;
  }
  for (index = 0; index < length; ++index) {
    path[index] = (uint8_t)unc[index];
  }
  memset(&request, 0, sizeof(request));
  request.path = path;
  request.path_length = (uint16_t)(length * sizeof(path[0]));
  memset(&state, 0, sizeof(state));
  state.status = -1;
  pdu = smb2_cmd_tree_connect_async(smb2, &request, raw_command_cb, &state);
  if (pdu == NULL) {
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  if (wait_raw_command(smb2, &state) != 0 ||
      (uint32_t)state.status != SMB2_STATUS_SUCCESS) {
    return -1;
  }
  *connected_tree_id = state.tree_id;
  return 0;
}

static int raw_query_directory_status(struct smb2_context *smb2,
                                      const smb2_file_id file_id,
                                      uint8_t information_class,
                                      uint32_t output_buffer_length,
                                      uint32_t *status) {
  struct raw_command_state state;
  struct smb2_query_directory_request request;
  struct smb2_pdu *pdu;
  if (file_id == NULL || status == NULL) {
    return -1;
  }
  memset(&state, 0, sizeof(state));
  state.status = -1;
  memset(&request, 0, sizeof(request));
  request.file_information_class = information_class;
  memcpy(request.file_id, file_id, sizeof(request.file_id));
  request.output_buffer_length = output_buffer_length;
  request.name = "*";
  pdu = smb2_cmd_query_directory_async(smb2, &request,
                                       raw_command_cb, &state);
  if (pdu == NULL) {
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  if (wait_raw_command(smb2, &state) != 0) {
    return -1;
  }
  *status = (uint32_t)state.status;
  return 0;
}

static void capture_compound_header(struct smb2_context *smb2,
                                    struct windows_compound_state *state,
                                    int index, int status) {
  state->status[index] = status;
  state->next_command[index] = smb2->hdr.next_command;
  state->flags[index] = smb2->hdr.flags;
  state->message_id[index] = smb2->hdr.message_id;
}

static int directory_padding_is_zero(const uint8_t *data, uint32_t length,
                                     int *entry_count) {
  uint32_t offset = 0;
  int entries = 0;
  if (entry_count == NULL) {
    return 0;
  }
  *entry_count = 0;
  while (offset < length) {
    uint32_t next;
    uint32_t name_length;
    uint32_t record_length;
    uint32_t used;
    uint32_t index;
    if (length - offset < SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE) {
      return 0;
    }
    next = test_read_le32(data + offset);
    name_length = test_read_le32(data + offset + 60);
    record_length = next == 0 ? length - offset : next;
    used = SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE + name_length;
    if (record_length > length - offset || record_length < used ||
        (next != 0 && (next & 7u) != 0)) {
      return 0;
    }
    for (index = used; index < record_length; ++index) {
      if (data[offset + index] != 0) {
        return 0;
      }
    }
    ++entries;
    if (next == 0) {
      break;
    }
    offset += next;
  }
  *entry_count = entries;
  return entries > 0;
}

static int capture_first_directory_entry(const uint8_t *data, uint32_t length,
                                         uint32_t *file_index, char *name,
                                         size_t name_capacity) {
  uint32_t name_length;
  size_t characters;
  size_t index;
  if (data == NULL || file_index == NULL || name == NULL ||
      name_capacity == 0 ||
      length < SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE) {
    return 0;
  }
  name_length = test_read_le32(data + 60);
  if ((name_length & 1u) != 0 ||
      name_length > length - SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE) {
    return 0;
  }
  *file_index = test_read_le32(data + 4);
  characters = name_length / 2;
  if (characters >= name_capacity) {
    characters = name_capacity - 1;
  }
  for (index = 0; index < characters; ++index) {
    const uint8_t *character =
        data + SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE + index * 2;
    name[index] = character[1] == 0 ? (char)character[0] : '?';
  }
  name[characters] = '\0';
  return 1;
}

static void raw_directory_cb(struct smb2_context *smb2, int status,
                             void *command_data, void *private_data) {
  struct raw_directory_state *state =
      (struct raw_directory_state*)private_data;
  struct smb2_query_directory_reply *reply =
      (struct smb2_query_directory_reply*)command_data;
  (void)smb2;
  state->status = status;
  if (state->event_counter != NULL) {
    state->final_order = ++*state->event_counter;
  }
  if ((uint32_t)status == SMB2_STATUS_SUCCESS && reply != NULL &&
      reply->output_buffer != NULL) {
    state->output_length = reply->output_buffer_length;
    state->padding_zero = directory_padding_is_zero(
        reply->output_buffer, reply->output_buffer_length,
        &state->entry_count);
    state->payload_valid = capture_first_directory_entry(
        reply->output_buffer, reply->output_buffer_length,
        &state->first_index, state->first_name, sizeof(state->first_name));
  }
  if (!state->done && state->completed_total != NULL) {
    ++*state->completed_total;
  }
  state->done = 1;
}

static int queue_raw_query_directory_entries(
    struct smb2_context *smb2, const smb2_file_id file_id, uint8_t flags,
    struct raw_directory_state *state) {
  struct smb2_query_directory_request request;
  struct smb2_pdu *pdu;
  if (smb2 == NULL || file_id == NULL || state == NULL) {
    return -1;
  }
  memset(state, 0, sizeof(*state));
  state->status = -1;
  memset(&request, 0, sizeof(request));
  request.file_information_class =
      SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION;
  request.flags = flags;
  memcpy(request.file_id, file_id, sizeof(request.file_id));
  request.output_buffer_length = 65536;
  request.name = "*";
  pdu = smb2_cmd_query_directory_async(
      smb2, &request, raw_directory_cb, state);
  if (pdu == NULL) {
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  return 0;
}

static int raw_query_directory_entries(struct smb2_context *smb2,
                                       const smb2_file_id file_id,
                                       uint8_t flags,
                                       struct raw_directory_state *state) {
  if (queue_raw_query_directory_entries(smb2, file_id, flags, state) != 0) {
    return -1;
  }
  return wait_for_count(smb2, &state->done, 1, 10000);
}

static void windows_compound_create_cb(struct smb2_context *smb2, int status,
                                       void *command_data,
                                       void *private_data) {
  struct windows_compound_state *state =
      (struct windows_compound_state*)private_data;
  struct smb2_create_reply *reply = (struct smb2_create_reply*)command_data;
  capture_compound_header(smb2, state, 0, status);
  if ((uint32_t)status == SMB2_STATUS_SUCCESS && reply != NULL) {
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
  ++state->completed;
}

static void windows_compound_directory_cb_index(
    struct smb2_context *smb2, int status, void *command_data,
    void *private_data, int index) {
  struct windows_compound_state *state =
      (struct windows_compound_state*)private_data;
  struct smb2_query_directory_reply *reply =
      (struct smb2_query_directory_reply*)command_data;
  capture_compound_header(smb2, state, index, status);
  if (index >= 1 && index <= 2 &&
      (uint32_t)status == SMB2_STATUS_SUCCESS &&
      reply != NULL && reply->output_buffer != NULL) {
    const int directory_index = index - 1;
    state->directory_output_length[directory_index] =
        reply->output_buffer_length;
    state->directory_payload_valid[directory_index] = 1;
    state->directory_padding_zero[directory_index] =
        directory_padding_is_zero(
            reply->output_buffer, reply->output_buffer_length,
            &state->directory_entry_count[directory_index]);
    state->directory_payload_valid[directory_index] =
        state->directory_payload_valid[directory_index] &&
        capture_first_directory_entry(
            reply->output_buffer, reply->output_buffer_length,
            &state->directory_first_index[directory_index],
            state->directory_first_name[directory_index],
            sizeof(state->directory_first_name[directory_index]));
  }
  ++state->completed;
}

static void windows_compound_first_directory_cb(
    struct smb2_context *smb2, int status, void *command_data,
    void *private_data) {
  windows_compound_directory_cb_index(
      smb2, status, command_data, private_data, 1);
}

static void windows_compound_second_directory_cb(
    struct smb2_context *smb2, int status, void *command_data,
    void *private_data) {
  windows_compound_directory_cb_index(
      smb2, status, command_data, private_data, 2);
}

static int inspect_windows_create_contexts(
    const struct windows_compound_state *state, uint32_t *query_status,
    uint32_t *maximal_access, uint64_t *disk_file_id,
    uint64_t *volume_id) {
  size_t offset = 0;
  int seen_mxac = 0;
  int seen_qfid = 0;
  int count = 0;
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
    ++count;
    if (name_length == 4 && memcmp(context + name_offset, "MxAc", 4) == 0) {
      if (seen_mxac || data_length != 8) {
        return -1;
      }
      *query_status = test_read_le32(context + data_offset);
      *maximal_access = test_read_le32(context + data_offset + 4);
      seen_mxac = 1;
    } else if (name_length == 4 &&
               memcmp(context + name_offset, "QFid", 4) == 0) {
      size_t index;
      if (seen_qfid || data_length != 32) {
        return -1;
      }
      *disk_file_id = test_read_le64(context + data_offset);
      *volume_id = test_read_le64(context + data_offset + 8);
      for (index = 16; index < 32; ++index) {
        if (context[data_offset + index] != 0) {
          return -1;
        }
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
  return seen_mxac && seen_qfid && count == 2 ? 0 : -1;
}

static void internal_id_cb(struct smb2_context *smb2, int status,
                           void *command_data, void *private_data) {
  struct internal_id_state *state =
      (struct internal_id_state*)private_data;
  struct smb2_query_info_reply *reply =
      (struct smb2_query_info_reply*)command_data;
  (void)smb2;
  state->status = status;
  if ((uint32_t)status == SMB2_STATUS_SUCCESS && reply != NULL &&
      reply->output_buffer != NULL && reply->output_buffer_length == 8) {
    state->index_number =
        test_read_le64((const uint8_t*)reply->output_buffer);
    state->decoded = 1;
  }
  state->done = 1;
}

static int query_internal_id(struct smb2_context *smb2,
                             const smb2_file_id file_id,
                             struct internal_id_state *state) {
  struct smb2_query_info_request request;
  struct smb2_pdu *pdu;
  int old_passthrough = 0;
  memset(&request, 0, sizeof(request));
  request.info_type = SMB2_0_INFO_FILE;
  request.file_info_class = SMB2_FILE_INTERNAL_INFORMATION;
  request.output_buffer_length = 8;
  memcpy(request.file_id, file_id, sizeof(request.file_id));
  memset(state, 0, sizeof(*state));
  smb2_get_passthrough(smb2, &old_passthrough);
  smb2_set_passthrough(smb2, 1);
  pdu = smb2_cmd_query_info_async(smb2, &request, internal_id_cb, state);
  if (pdu == NULL) {
    smb2_set_passthrough(smb2, old_passthrough);
    return -1;
  }
  smb2_queue_pdu(smb2, pdu);
  {
    const int result = wait_for_count(smb2, &state->done, 1, 10000);
    smb2_set_passthrough(smb2, old_passthrough);
    return result;
  }
}

static int run_windows_directory_compound(
    struct smb2_context *smb2, const char *directory,
    struct windows_compound_state *state, struct smb2_context *echo_peer,
    DWORD *echo_elapsed_ms) {
  struct smb2_create_request create_request;
  struct smb2_query_directory_request first_query;
  struct smb2_query_directory_request second_query;
  struct smb2_pdu *head;
  struct smb2_pdu *next;
  uint8_t contexts[48];
  int old_passthrough = 0;

  memset(contexts, 0, sizeof(contexts));
  test_write_le32(contexts, 24);
  test_write_le16(contexts + 4, 16);
  test_write_le16(contexts + 6, 4);
  test_write_le16(contexts + 10, 24);
  memcpy(contexts + 16, "MxAc", 4);
  test_write_le16(contexts + 24 + 4, 16);
  test_write_le16(contexts + 24 + 6, 4);
  test_write_le16(contexts + 24 + 10, 24);
  memcpy(contexts + 24 + 16, "QFid", 4);

  memset(state, 0, sizeof(*state));
  memset(&create_request, 0, sizeof(create_request));
  create_request.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
  create_request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  create_request.desired_access = SMB2_FILE_READ_DATA |
                                  SMB2_FILE_READ_ATTRIBUTES |
                                  SMB2_SYNCHRONIZE;
  create_request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE |
                                SMB2_FILE_SHARE_DELETE;
  create_request.create_disposition = SMB2_FILE_OPEN;
  create_request.create_options = SMB2_FILE_DIRECTORY_FILE |
                                  SMB2_FILE_SYNCHRONOUS_IO_NONALERT;
  create_request.name = directory;
  create_request.create_context_length = sizeof(contexts);
  create_request.create_context = contexts;

  memset(&first_query, 0, sizeof(first_query));
  first_query.file_information_class =
      SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION;
  memcpy(first_query.file_id, compound_file_id, SMB2_FD_SIZE);
  first_query.output_buffer_length = 65536;
  first_query.name = "*";

  memset(&second_query, 0, sizeof(second_query));
  second_query.file_information_class =
      SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION;
  memcpy(second_query.file_id, compound_file_id, SMB2_FD_SIZE);
  second_query.output_buffer_length = 1024;
  second_query.name = "*";

  smb2_get_passthrough(smb2, &old_passthrough);
  smb2_set_passthrough(smb2, 1);

  head = smb2_cmd_create_async(smb2, &create_request,
                               windows_compound_create_cb, state);
  if (head == NULL) {
    smb2_set_passthrough(smb2, old_passthrough);
    return -1;
  }
  next = smb2_cmd_query_directory_async(
      smb2, &first_query, windows_compound_first_directory_cb, state);
  if (next == NULL) {
    smb2_free_pdu(smb2, head);
    smb2_set_passthrough(smb2, old_passthrough);
    return -1;
  }
  smb2_add_compound_pdu(smb2, head, next);
  next = smb2_cmd_query_directory_async(
      smb2, &second_query, windows_compound_second_directory_cb, state);
  if (next == NULL) {
    smb2_free_pdu(smb2, head);
    smb2_set_passthrough(smb2, old_passthrough);
    return -1;
  }
  smb2_add_compound_pdu(smb2, head, next);
  state->request_next_command[0] = head->header.next_command;
  state->request_next_command[1] =
      head->next_compound->header.next_command;
  state->request_next_command[2] =
      head->next_compound->next_compound->header.next_command;
  state->request_flags[0] = head->header.flags;
  state->request_flags[1] = head->next_compound->header.flags;
  state->request_flags[2] =
      head->next_compound->next_compound->header.flags;
  smb2_queue_pdu(smb2, head);
  if (echo_peer != NULL) {
    const DWORD started = GetTickCount();
    /* Даём серверу принять compound и начать медленный FINDNEXT. На
     * корректном async_internal другой SMB-сеанс отвечает немедленно. */
    Sleep(50);
    if (smb2_echo(echo_peer) != 0) {
      smb2_set_passthrough(smb2, old_passthrough);
      return -1;
    }
    if (echo_elapsed_ms != NULL) {
      *echo_elapsed_ms = GetTickCount() - started;
    }
  }
  {
    const int result = wait_for_count(smb2, &state->completed, 3, 10000);
    if (result != 0) {
      printf("  Compound service failed after %d callbacks: %s\n",
             state->completed, smb2_get_error(smb2));
    }
    smb2_set_passthrough(smb2, old_passthrough);
    return result;
  }
}

static void query_info_cb(struct smb2_context *smb2, int status,
                          void *command_data, void *private_data) {
  struct query_info_state *state = (struct query_info_state*)private_data;
  (void)smb2;
  (void)command_data;
  state->status = status;
  state->done = 1;
}

static void allocation_query_cb(struct smb2_context *smb2, int status,
                                void *command_data, void *private_data) {
  struct allocation_query_state *state =
      (struct allocation_query_state*)private_data;
  state->status = status;
  if (status == SMB2_STATUS_SUCCESS && command_data != NULL) {
    struct smb2_query_info_reply *reply =
        (struct smb2_query_info_reply*)command_data;
    if (reply->output_buffer != NULL) {
      switch (state->information_class) {
        case SMB2_FILE_STANDARD_INFORMATION: {
          const struct smb2_file_standard_info *info =
              (const struct smb2_file_standard_info*)reply->output_buffer;
          state->allocation_size = info->allocation_size;
          state->end_of_file = info->end_of_file;
          break;
        }
        case SMB2_FILE_ALL_INFORMATION: {
          const struct smb2_file_all_info *info =
              (const struct smb2_file_all_info*)reply->output_buffer;
          state->allocation_size = info->standard.allocation_size;
          state->end_of_file = info->standard.end_of_file;
          break;
        }
        case SMB2_FILE_NETWORK_OPEN_INFORMATION: {
          const struct smb2_file_network_open_info *info =
              (const struct smb2_file_network_open_info*)reply->output_buffer;
          state->allocation_size = info->allocation_size;
          state->end_of_file = info->end_of_file;
          break;
        }
        case SMB2_FILE_STREAM_INFORMATION: {
          const struct smb2_file_stream_info *info =
              (const struct smb2_file_stream_info*)reply->output_buffer;
          state->allocation_size = info->stream_allocation_size;
          state->end_of_file = info->stream_size;
          break;
        }
      }
      smb2_free_data(smb2, reply->output_buffer);
    }
  }
  state->done = 1;
}

static int query_file_allocation(struct smb2_context *smb2,
                                 struct smb2fh *handle,
                                 uint8_t information_class,
                                 uint64_t *allocation_size,
                                 uint64_t *end_of_file) {
  struct probe_fh_mirror *mirror = (struct probe_fh_mirror*)handle;
  struct smb2_query_info_request request;
  struct allocation_query_state state;
  struct smb2_pdu *pdu;
  const DWORD started = GetTickCount();

  memset(&request, 0, sizeof(request));
  request.info_type = SMB2_0_INFO_FILE;
  request.file_info_class = information_class;
  request.output_buffer_length = 65536;
  memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
  memset(&state, 0, sizeof(state));
  state.status = -1;
  state.information_class = information_class;

  pdu = smb2_cmd_query_info_async(smb2, &request, allocation_query_cb,
                                  &state);
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
  if (state.status != SMB2_STATUS_SUCCESS) {
    return -1;
  }
  *allocation_size = state.allocation_size;
  *end_of_file = state.end_of_file;
  return 0;
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

static int raw_lock_status(struct smb2_context *smb2, struct smb2fh *handle,
                           uint64_t offset, uint64_t length, uint32_t flags,
                           uint32_t *status) {
  struct probe_fh_mirror *mirror = (struct probe_fh_mirror*)handle;
  struct smb2_lock_element element;
  struct smb2_lock_request request;
  struct raw_command_state state;
  struct smb2_pdu *pdu;
  if (mirror == NULL || status == NULL) return -1;
  memset(&element, 0, sizeof(element));
  element.offset = offset;
  element.length = length;
  element.flags = flags;
  memset(&request, 0, sizeof(request));
  request.lock_count = 1;
  request.locks = &element;
  memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
  memset(&state, 0, sizeof(state));
  state.status = -1;
  pdu = smb2_cmd_lock_async(smb2, &request, raw_command_cb, &state);
  if (pdu == NULL) return -1;
  smb2_queue_pdu(smb2, pdu);
  if (wait_raw_command(smb2, &state) != 0) return -1;
  *status = (uint32_t)state.status;
  return 0;
}

static int raw_read_status(struct smb2_context *smb2, struct smb2fh *handle,
                           uint64_t offset, uint8_t *buffer, uint32_t length,
                           uint32_t *status) {
  struct probe_fh_mirror *mirror = (struct probe_fh_mirror*)handle;
  struct smb2_read_request request;
  struct raw_command_state state;
  struct smb2_pdu *pdu;
  if (mirror == NULL || buffer == NULL || length == 0 || status == NULL) {
    return -1;
  }
  memset(&request, 0, sizeof(request));
  request.offset = offset;
  request.length = length;
  request.buf = buffer;
  memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
  memset(&state, 0, sizeof(state));
  state.status = -1;
  pdu = smb2_cmd_read_async(smb2, &request, raw_command_cb, &state);
  if (pdu == NULL) return -1;
  smb2_queue_pdu(smb2, pdu);
  if (wait_raw_command(smb2, &state) != 0) return -1;
  *status = (uint32_t)state.status;
  return 0;
}

static int raw_write_status(struct smb2_context *smb2,
                            struct smb2fh *handle, uint64_t offset,
                            const uint8_t *buffer, uint32_t length,
                            uint32_t *status) {
  struct probe_fh_mirror *mirror = (struct probe_fh_mirror*)handle;
  struct smb2_write_request request;
  struct raw_command_state state;
  struct smb2_pdu *pdu;
  if (mirror == NULL || buffer == NULL || length == 0 || status == NULL) {
    return -1;
  }
  memset(&request, 0, sizeof(request));
  request.offset = offset;
  request.length = length;
  request.buf = buffer;
  memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
  memset(&state, 0, sizeof(state));
  state.status = -1;
  pdu = smb2_cmd_write_async(smb2, &request, 0,
                             raw_command_cb, &state);
  if (pdu == NULL) return -1;
  smb2_queue_pdu(smb2, pdu);
  if (wait_raw_command(smb2, &state) != 0) return -1;
  *status = (uint32_t)state.status;
  return 0;
}

static int queue_raw_change_notify(struct smb2_context *smb2,
                                   const smb2_file_id file_id,
                                   struct raw_change_notify_state *state,
                                   int *completed_total) {
  struct smb2_change_notify_request request;
  struct smb2_pdu *pdu;
  if (file_id == NULL || state == NULL || completed_total == NULL) return -1;
  memset(&request, 0, sizeof(request));
  request.flags = SMB2_CHANGE_NOTIFY_WATCH_TREE;
  request.output_buffer_length = 4096;
  request.completion_filter =
      SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_FILE_NAME |
      SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_DIR_NAME |
      SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_WRITE;
  memcpy(request.file_id, file_id, sizeof(request.file_id));
  memset(state, 0, sizeof(*state));
  state->completed_total = completed_total;
  state->final_status = -1;
  pdu = smb2_cmd_change_notify_async(smb2, &request,
                                     raw_change_notify_cb, state);
  if (pdu == NULL) return -1;
  smb2_queue_pdu(smb2, pdu);
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

int main(int argc, char **argv) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
    printf("FAIL: WSAStartup failed\n");
    return 1;
  }

  if (argc < 3) {
    printf("Usage: smb_reproduce_test host[:port] share [all|basic|test8|test9|test10|test11|test12|test13|test14|test15|test16|test17|test18|test19|test20|test21|test22|test23|test24|test25|test26|test27|test28|test29|test30|test31|test32|test33] [test-directory]\n");
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
  /* Сервер начинает с одного кредита по MS-SMB2 и расширяет окно до четырёх:
   * это постоянное окно обратного давления для одного FILEX-сервиса. */
  {
    const int credit_ok = smb2->credits == SMB2_SERVER_CREDIT_TARGET;
    printf("NEGOTIATED CREDITS: %d (%s)\n", smb2->credits,
           credit_ok ? "PASS" : "FAIL: expected bounded window of 4");
    failures += !credit_ok;
  }

  /* Повторные ответы должны возвращать только израсходованный кредит. Если
   * каждый ответ снова выдаст все запрошенные кредиты, окно быстро накопится
   * выше физического пула. */
  {
    int credit_ok = 1;
    uint16_t min_grant = 0xffff;
    uint16_t max_grant = 0;
    for (int iteration = 0; iteration < 32; ++iteration) {
      if (smb2_echo(smb2) != 0) {
        credit_ok = 0;
        break;
      }
      if (smb2->hdr.credit_request_response < min_grant) {
        min_grant = smb2->hdr.credit_request_response;
      }
      if (smb2->hdr.credit_request_response > max_grant) {
        max_grant = smb2->hdr.credit_request_response;
      }
      if (smb2->hdr.credit_request_response != 1 ||
          smb2->credits != SMB2_SERVER_CREDIT_TARGET) {
        credit_ok = 0;
        break;
      }
    }
    printf("CREDIT WINDOW: held=%d grants=%u..%u after 32 ECHO (%s)\n",
           smb2->credits, (unsigned)min_grant, (unsigned)max_grant,
           credit_ok ? "PASS" : "FAIL");
    failures += !credit_ok;
  }

  /* Samba smbd строит стабильный ServerGuid из NetBIOS-имени. Все соединения
   * ZX-Evo должны видеть ровно "zx-evo" с нулевым хвостом, а SessionId
   * создаётся отдельно для каждого аутентифицированного соединения. */
  {
    static const unsigned char expected_guid[16] = {
      'z', 'x', '-', 'e', 'v', 'o', 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0
    };
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
    const int guid_ok = server_guid != NULL && peer_guid != NULL &&
                        memcmp(server_guid, peer_guid, 16) == 0 &&
                        memcmp(server_guid, expected_guid, 16) == 0;
    const int session_ok = have_session && peer_has_session &&
                           session_id != 0 && peer_session_id != 0 &&
                           session_id != 0x1234 &&
                           peer_session_id != 0x1234 &&
                           session_id != peer_session_id;
    printf("SERVER IDENTITY: guid=");
    if (server_guid != NULL) {
      int index;
      for (index = 0; index < 16; ++index) {
        printf("%02x", (unsigned)server_guid[index]);
      }
    } else {
      printf("missing");
    }
    printf(" samba-stable/shared=%s sessions=0x%016llx/0x%016llx (%s)\n",
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
    if (strcmp(only_test, "test18") == 0) goto test_18;
    if (strcmp(only_test, "test19") == 0) goto test_19;
    if (strcmp(only_test, "test20") == 0) goto test_20;
    if (strcmp(only_test, "test21") == 0) goto test_21;
    if (strcmp(only_test, "test22") == 0) goto test_22;
    if (strcmp(only_test, "test23") == 0) goto test_23;
    if (strcmp(only_test, "test24") == 0) goto test_24;
    if (strcmp(only_test, "test25") == 0) goto test_25;
    if (strcmp(only_test, "test26") == 0) goto test_26;
    if (strcmp(only_test, "test27") == 0) goto test_27;
    if (strcmp(only_test, "test28") == 0) goto test_28;
    if (strcmp(only_test, "test29") == 0) goto test_29;
    if (strcmp(only_test, "test30") == 0) goto test_30;
    if (strcmp(only_test, "test31") == 0) goto test_31;
    if (strcmp(only_test, "test32") == 0) goto test_32;
    if (strcmp(only_test, "test33") == 0) goto test_33;
    if (strcmp(only_test, "test34") == 0) goto test_34;
    if (strcmp(only_test, "test35") == 0) goto test_35;
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
  /* TEST 17: Explorer выбирает объект из QUERY_DIRECTORY, а затем открывает
   * его и запрашивает FILE_INTERNAL_INFORMATION. Оба пути обязаны вернуть
   * один FileId; иначе Shell не начинает чтение выбранного файла. */
  printf("\n--- TEST 17: Directory and open FileId identity ---\n");
  {
    const char *file_name = "file_identity.bin";
    char file_path[512];
    struct smb2fh *writer = NULL;
    struct smb2dir *directory = NULL;
    struct smb2dirent *entry = NULL;
    struct smb2_stat_64 opened_stat;
    uint64_t directory_id = 0;
    int found = 0;
    int test_ok = 1;
    const uint8_t byte = 0xa5;

    snprintf(file_path, sizeof(file_path), "%s", test_path(file_name));
    writer = smb2_open(smb2, file_path, O_CREAT | O_TRUNC | O_WRONLY);
    test_ok = writer != NULL;
    if (test_ok) {
      test_ok = smb2_write(smb2, writer, (void*)&byte, 1) == 1;
      test_ok = smb2_close(smb2, writer) == 0 && test_ok;
      writer = NULL;
    }
    if (test_ok) {
      directory = smb2_opendir(smb2, test_directory);
      test_ok = directory != NULL;
    }
    while (directory != NULL &&
           (entry = smb2_readdir(smb2, directory)) != NULL) {
      if (strcmp(entry->name, file_name) == 0) {
        directory_id = entry->st.smb2_ino;
        found = 1;
        break;
      }
    }
    if (directory != NULL) {
      smb2_closedir(smb2, directory);
    }
    memset(&opened_stat, 0, sizeof(opened_stat));
    if (test_ok) {
      test_ok = found && smb2_stat(smb2, file_path, &opened_stat) == 0;
    }
    printf("  Directory FileId=0x%016llx open FileId=0x%016llx found=%d\n",
           (unsigned long long)directory_id,
           (unsigned long long)opened_stat.smb2_ino, found);
    test_ok = test_ok && directory_id != 0 &&
              directory_id == opened_stat.smb2_ino;
    if (writer != NULL) {
      smb2_close(smb2, writer);
    }
    printf("RESULT TEST 17: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_18:
  /* TEST 18: MS-FSCC требует, чтобы AllocationSize был кратен размеру
   * кластера тома. Эталонный случай повторяет реальный зависший CopyFile:
   * EOF=262147, FAT32 cluster=32768, значит AllocationSize=294912, а не
   * прежние 262656 (округление до сектора 512). */
  printf("\n--- TEST 18: AllocationSize follows FAT32 cluster geometry ---\n");
  {
    static const uint8_t classes[] = {
        SMB2_FILE_STANDARD_INFORMATION,
        SMB2_FILE_ALL_INFORMATION,
        SMB2_FILE_NETWORK_OPEN_INFORMATION,
        SMB2_FILE_STREAM_INFORMATION};
    const uint32_t file_size = 262147;
    const uint64_t expected_allocation = 294912;
    uint8_t *buffer = (uint8_t*)calloc(1, 65536);
    struct smb2fh *handle = NULL;
    struct smb2_statvfs statvfs;
    uint32_t written_total = 0;
    int test_ok = buffer != NULL;

    memset(&statvfs, 0, sizeof(statvfs));
    if (test_ok) {
      test_ok = smb2_statvfs(smb2, test_directory, &statvfs) == 0 &&
                statvfs.f_frsize == 32768;
      printf("  Filesystem allocation unit=%lu expected=32768\n",
             (unsigned long)statvfs.f_frsize);
    }
    if (test_ok) {
      handle = smb2_open(smb2, test_path("allocation_262147.bin"),
                         O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = handle != NULL;
    }
    while (test_ok && written_total < file_size) {
      uint32_t part = file_size - written_total;
      if (part > 65536) part = 65536;
      const int written = smb2_write(smb2, handle, buffer, part);
      if (written != (int)part) {
        test_ok = 0;
      } else {
        written_total += part;
      }
    }
    if (handle != NULL) {
      test_ok = smb2_close(smb2, handle) == 0 && test_ok;
      handle = NULL;
    }
    if (test_ok) {
      handle = smb2_open(smb2, test_path("allocation_262147.bin"), O_RDONLY);
      test_ok = handle != NULL;
    }
    for (size_t index = 0;
         test_ok && index < sizeof(classes) / sizeof(classes[0]); ++index) {
      uint64_t allocation = 0;
      uint64_t eof = 0;
      test_ok = query_file_allocation(smb2, handle, classes[index],
                                      &allocation, &eof) == 0 &&
                allocation == expected_allocation && eof == file_size;
      printf("  Class %u: EOF=%llu AllocationSize=%llu (%s)\n",
             (unsigned)classes[index], (unsigned long long)eof,
             (unsigned long long)allocation, test_ok ? "PASS" : "FAIL");
    }
    if (handle != NULL) {
      test_ok = smb2_close(smb2, handle) == 0 && test_ok;
    }
    free(buffer);
    printf("RESULT TEST 18: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_19:
  /* TEST 19: точная форма запроса Проводника при открытии каталога. Сервер
   * обязан вернуть CREATE + оба QUERY_DIRECTORY в одном составном ответе.
   * QD не содержат RETURN_SINGLE_ENTRY; первый предоставляет 65536 байт,
   * второй — 1024. На медленном UART оба должны вернуть ограниченный пакет,
   * а не ждать EOF всего каталога. Также проверяются связанный FileId и
   * нулевое выравнивание. Запрос содержит MxAc и QFid. Ответ QFid обязан
   * содержать тот же DiskFileId, который последующий QUERY_INFO возвращает
   * как FileInternalInformation, и ненулевой идентификатор тома FAT. */
  printf("\n--- TEST 19: Windows related hardware-bounded directory compound ---\n");
  {
    const char *directory = test_directory[0] == '\0' ? "" : test_directory;
    struct windows_compound_state state;
    struct internal_id_state internal;
    struct smb2fh *marker = NULL;
    struct smb2fh *directory_handle = NULL;
    struct smb2_context *echo_peer = NULL;
    DWORD echo_elapsed_ms = 0;
    uint32_t query_status = 0xffffffffu;
    uint32_t maximal_access = 0;
    uint64_t disk_file_id = 0;
    uint64_t volume_id = 0;
    const uint8_t marker_byte = 0x5a;
    int seed_index;
    int header_ok;
    int test_ok;

    memset(&state, 0, sizeof(state));
    memset(&internal, 0, sizeof(internal));
    marker = smb2_open(smb2, test_path("compound_padding_odd.bin"),
                       O_CREAT | O_TRUNC | O_WRONLY);
    test_ok = marker != NULL;
    if (test_ok) {
      test_ok = smb2_write(smb2, marker, (void*)&marker_byte, 1) == 1;
    }
    if (marker != NULL) {
      test_ok = smb2_close(smb2, marker) == 0 && test_ok;
      marker = NULL;
    }
    for (seed_index = 0; test_ok && seed_index < 40; ++seed_index) {
      char name[64];
      struct smb2fh *seed;
      snprintf(name, sizeof(name), "directory_batch_%02d.tmp", seed_index);
      seed = smb2_open(smb2, test_path(name), O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = seed != NULL;
      if (seed != NULL) {
        test_ok = smb2_close(smb2, seed) == 0 && test_ok;
      }
    }
    if (test_ok) {
      echo_peer = connect_context(server, share);
      test_ok = echo_peer != NULL;
    }
    if (test_ok) {
      test_ok = run_windows_directory_compound(
                    smb2, directory, &state, echo_peer,
                    &echo_elapsed_ms) == 0;
    }
    header_ok = state.completed == 3 &&
        state.request_next_command[0] == 176 &&
        state.request_next_command[1] == 104 &&
        state.request_next_command[2] == 0 &&
        (state.request_flags[0] & SMB2_FLAGS_RELATED_OPERATIONS) == 0 &&
        (state.request_flags[1] & SMB2_FLAGS_RELATED_OPERATIONS) != 0 &&
        (state.request_flags[2] & SMB2_FLAGS_RELATED_OPERATIONS) != 0 &&
        state.next_command[0] != 0 &&
        (state.next_command[0] & 7u) == 0 &&
        state.next_command[1] != 0 &&
        (state.next_command[1] & 7u) == 0 &&
        state.next_command[2] == 0 &&
        (state.flags[0] & SMB2_FLAGS_SERVER_TO_REDIR) != 0 &&
        (state.flags[0] & SMB2_FLAGS_RELATED_OPERATIONS) == 0 &&
        (state.flags[1] & (SMB2_FLAGS_SERVER_TO_REDIR |
                           SMB2_FLAGS_RELATED_OPERATIONS)) ==
            (SMB2_FLAGS_SERVER_TO_REDIR | SMB2_FLAGS_RELATED_OPERATIONS) &&
        (state.flags[2] & (SMB2_FLAGS_SERVER_TO_REDIR |
                           SMB2_FLAGS_RELATED_OPERATIONS)) ==
            (SMB2_FLAGS_SERVER_TO_REDIR | SMB2_FLAGS_RELATED_OPERATIONS) &&
        state.message_id[1] == state.message_id[0] + 1 &&
        state.message_id[2] == state.message_id[1] + 1;
    test_ok = test_ok && header_ok &&
        (uint32_t)state.status[0] == SMB2_STATUS_SUCCESS &&
        (uint32_t)state.status[1] == SMB2_STATUS_SUCCESS &&
        (uint32_t)state.status[2] == SMB2_STATUS_SUCCESS &&
        state.oplock_level == SMB2_OPLOCK_LEVEL_NONE &&
        state.directory_payload_valid[0] &&
        state.directory_payload_valid[1] &&
        state.directory_padding_zero[0] &&
        state.directory_padding_zero[1] &&
        /* Холодный FILEX-путь не должен удерживать compound ради пакетного
         * чтения: по одной записи на каждый из двух QUERY_DIRECTORY. */
        state.directory_entry_count[0] == 1 &&
        state.directory_entry_count[1] == 1 &&
        state.directory_output_length[0] <= 65536 &&
        state.directory_output_length[1] <= 1024 &&
        echo_elapsed_ms < 300;

    if (test_ok) {
      test_ok = inspect_windows_create_contexts(
                    &state, &query_status, &maximal_access,
                    &disk_file_id, &volume_id) == 0;
    }
    if (test_ok) {
      test_ok = query_internal_id(smb2, state.file_id, &internal) == 0 &&
                (uint32_t)internal.status == SMB2_STATUS_SUCCESS &&
                internal.decoded && internal.index_number == disk_file_id;
    }
    if ((uint32_t)state.status[0] == SMB2_STATUS_SUCCESS) {
      directory_handle = smb2_fh_from_file_id(smb2, &state.file_id);
      if (directory_handle == NULL ||
          smb2_close(smb2, directory_handle) != 0) {
        test_ok = 0;
      }
    }
    test_ok = test_ok && query_status == SMB2_STATUS_SUCCESS &&
              maximal_access == 0x001F01FFu && disk_file_id != 0 &&
              volume_id != 0;

    printf("  Status CREATE/QD/QD=%08x/%08x/%08x callbacks=%d\n",
           (unsigned)state.status[0], (unsigned)state.status[1],
           (unsigned)state.status[2], state.completed);
    printf("  Request NextCommand=%lu/%lu/%lu (capture=176/104/0)\n",
           (unsigned long)state.request_next_command[0],
           (unsigned long)state.request_next_command[1],
           (unsigned long)state.request_next_command[2]);
    printf("  NextCommand=%lu/%lu/%lu flags=%08x/%08x/%08x (%s)\n",
           (unsigned long)state.next_command[0],
           (unsigned long)state.next_command[1],
           (unsigned long)state.next_command[2],
           (unsigned)state.flags[0], (unsigned)state.flags[1],
           (unsigned)state.flags[2], header_ok ? "COMPOUND" : "SPLIT");
    printf("  Directory bytes=%lu/%lu entries=%d/%d zero-padding=%s/%s\n",
           (unsigned long)state.directory_output_length[0],
           (unsigned long)state.directory_output_length[1],
           state.directory_entry_count[0], state.directory_entry_count[1],
           state.directory_padding_zero[0] ? "PASS" : "FAIL",
           state.directory_padding_zero[1] ? "PASS" : "FAIL");
    printf("  Parallel ECHO during FILEX directory=%lu ms (%s)\n",
           (unsigned long)echo_elapsed_ms,
           echo_elapsed_ms < 300 ? "PASS" : "FAIL");
    printf("  MxAc=%08x/%08x\n",
           (unsigned)query_status, (unsigned)maximal_access);
    printf("  QFid=0x%016llx Internal=0x%016llx Volume=0x%016llx\n",
           (unsigned long long)disk_file_id,
           (unsigned long long)internal.index_number,
           (unsigned long long)volume_id);
    printf("RESULT TEST 19: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
    if (echo_peer != NULL) {
      smb2_disconnect_share(echo_peer);
      smb2_destroy_context(echo_peer);
    }
  }

  if (only_test != NULL) goto done;

test_20:
  /* TEST 20: клиент ставит 64 READ, но сервер физически обслуживает один.
   * Ни активный, ни ожидающий READ не должен получать STATUS_PENDING: каждый
   * удерживает свой credit до синхронного финала. Поэтому клиентская очередь не
   * превращается в самопополняющийся конвейер, который игнорирует отмену. */
  printf("\n--- TEST 20: bounded synchronous READ credit window ---\n");
  {
    const uint32_t chunk_size = 65536;
    const int read_count = 64;
    const uint32_t file_size = (read_count - 1) * chunk_size + 15704;
    struct raw_async_read_state states[64];
    uint8_t *write_buffer = (uint8_t*)malloc(chunk_size);
    struct smb2fh *write_handle = NULL;
    struct smb2fh *read_handle = NULL;
    struct probe_fh_mirror *mirror = NULL;
    int completed = 0;
    int event_counter = 0;
    int queued = 0;
    int old_passthrough = 0;
    int test_ok = write_buffer != NULL;
    int index;

    memset(states, 0, sizeof(states));
    if (test_ok) {
      write_handle = smb2_open(smb2, test_path("async_pending_many.bin"),
                               O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = write_handle != NULL;
    }
    for (uint32_t offset = 0; test_ok && offset < file_size;
         offset += chunk_size) {
      uint32_t length = file_size - offset;
      if (length > chunk_size) length = chunk_size;
      for (uint32_t byte = 0; byte < length; ++byte) {
        write_buffer[byte] = test_byte((uint64_t)offset + byte);
      }
      if (smb2_write(smb2, write_handle, write_buffer, length) !=
          (int)length) {
        test_ok = 0;
      }
    }
    if (write_handle != NULL) {
      test_ok = smb2_close(smb2, write_handle) == 0 && test_ok;
      write_handle = NULL;
    }
    free(write_buffer);

    if (test_ok) {
      read_handle = smb2_open(smb2, test_path("async_pending_many.bin"),
                              O_RDONLY);
      test_ok = read_handle != NULL;
    }
    if (test_ok) {
      mirror = (struct probe_fh_mirror*)read_handle;
      smb2_get_passthrough(smb2, &old_passthrough);
      smb2_set_passthrough(smb2, 1);
    }
    for (index = 0; test_ok && index < read_count; ++index) {
      struct smb2_read_request request;
      struct smb2_pdu *pdu;
      const uint64_t offset = (uint64_t)index * chunk_size;
      uint32_t expected = file_size - (uint32_t)offset;
      if (expected > chunk_size) expected = chunk_size;
      states[index].buffer = (uint8_t*)malloc(chunk_size);
      states[index].expected_length = expected;
      states[index].offset = offset;
      states[index].completed_total = &completed;
      states[index].event_counter = &event_counter;
      states[index].final_status = -1;
      if (states[index].buffer == NULL) {
        test_ok = 0;
        break;
      }
      memset(&request, 0, sizeof(request));
      request.length = chunk_size;
      request.offset = offset;
      request.buf = states[index].buffer;
      memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
      pdu = smb2_cmd_read_async(smb2, &request, raw_async_read_cb,
                                &states[index]);
      if (pdu == NULL) {
        test_ok = 0;
        break;
      }
      smb2_queue_pdu(smb2, pdu);
      ++queued;
    }
    if (queued != 0 &&
        wait_for_count(smb2, &completed, queued, 360000) != 0) {
      printf("  Timeout/service failure: %s\n", smb2_get_error(smb2));
      test_ok = 0;
    }
    test_ok = test_ok && queued == read_count;
    if (mirror != NULL) {
      smb2_set_passthrough(smb2, old_passthrough);
    }

    for (index = 0; index < read_count; ++index) {
      const struct raw_async_read_state *state = &states[index];
      if (!state->completed || state->pending_count != 0 ||
          state->final_count != 1 ||
          (uint32_t)state->final_status != SMB2_STATUS_SUCCESS ||
          state->corrupted != 0 ||
          state->final_command != SMB2_READ ||
          (state->final_flags & SMB2_FLAGS_SERVER_TO_REDIR) == 0 ||
          (state->final_flags & SMB2_FLAGS_ASYNC_COMMAND) != 0 ||
          state->final_next_command != 0 || state->final_credit == 0) {
        printf("  READ %d contract violation: completed=%d callbacks=%d/%d "
               "status=%08x command=%u flags=%08x next=%lu credit=%u\n",
               index, state->completed, state->pending_count,
               state->final_count, (unsigned)state->final_status,
               (unsigned)state->final_command, (unsigned)state->final_flags,
               (unsigned long)state->final_next_command,
               (unsigned)state->final_credit);
        test_ok = 0;
      }
      printf("  READ %d off=%llu pending=%d final=%08x mid=0x%016llx "
             "credit=%u bytes=%lu bad=%d order=%d\n",
             index, (unsigned long long)state->offset, state->pending_count,
             (unsigned)state->final_status,
             (unsigned long long)state->final_message_id,
             (unsigned)state->final_credit,
             (unsigned long)state->expected_length, state->corrupted,
             state->final_order);
      free(states[index].buffer);
    }
    /* При глубине физического сервиса 1 финалы обязаны идти по смещениям, а
     * промежуточных ответов, возвращающих credit до финала, быть не должно. */
    {
      int bounded_window_ok = states[0].final_order > 0;
      for (index = 1; index < read_count; ++index) {
        bounded_window_ok = bounded_window_ok &&
                            states[index].pending_count == 0 &&
                            states[index].final_order >
                                states[index - 1].final_order;
      }
      printf("  Synchronous bounded window: %s (READ0 final=%d, "
             "READ%d final=%d)\n",
             bounded_window_ok ? "PASS" : "FAIL", states[0].final_order,
             read_count - 1, states[read_count - 1].final_order);
      test_ok = test_ok && bounded_window_ok;
    }
    if (read_handle != NULL) {
      test_ok = smb2_close(smb2, read_handle) == 0 && test_ok;
    }
    printf("RESULT TEST 20: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_21:
  /* ТЕСТ 21: MS-SMB2 3.3.5.8 требует закрыть все открытые объекты, связанные
   * с TreeConnect, до удаления дерева. Windows держит TCP и Session дольше
   * отдельного подключения к ресурсу,
   * поэтому проверяем повторный TREE_CONNECT в том же сеансе, не маскируя
   * утечку последующим LOGOFF или разрушением транспорта. */
  printf("\n--- TEST 21: TREE_DISCONNECT closes every open of that tree ---\n");
  {
    struct smb2fh *seed = NULL;
    struct smb2fh *leaked[8];
    struct smb2fh *reopened = NULL;
    const char *path = test_path("tree_disconnect_handles.bin");
    const uint8_t byte = 0x5a;
    uint32_t old_tree_id = 0;
    uint32_t new_tree_id = 0;
    int opened = 0;
    int test_ok = 1;
    int index;

    memset(leaked, 0, sizeof(leaked));
    seed = smb2_open(smb2, path, O_CREAT | O_TRUNC | O_WRONLY);
    test_ok = seed != NULL;
    if (test_ok) {
      test_ok = smb2_write(smb2, seed, (void*)&byte, 1) == 1;
    }
    if (seed != NULL) {
      test_ok = smb2_close(smb2, seed) == 0 && test_ok;
      seed = NULL;
    }
    for (index = 0; test_ok && index < 8; ++index) {
      leaked[index] = smb2_open(smb2, path, O_RDONLY);
      test_ok = leaked[index] != NULL;
      opened += leaked[index] != NULL;
    }
    if (test_ok) {
      test_ok = raw_tree_disconnect(smb2, &old_tree_id) == 0;
    }
    if (test_ok) {
      test_ok = raw_tree_connect(smb2, share, &new_tree_id) == 0 &&
                new_tree_id != 0 && new_tree_id != old_tree_id;
    }
    if (test_ok) {
      reopened = smb2_open(smb2, path, O_RDONLY);
      test_ok = reopened != NULL;
    }
    printf("  Kept opens=%d old-tree=0x%08x new-tree=0x%08x reopen=%s error=%s\n",
           opened, (unsigned)old_tree_id, (unsigned)new_tree_id,
           reopened != NULL ? "PASS" : "FAIL",
           reopened != NULL ? "" : smb2_get_error(smb2));
    if (reopened != NULL) {
      test_ok = smb2_close(smb2, reopened) == 0 && test_ok;
    }
    /* leaked[] принадлежит отключённому дереву. На новом дереве эти
     * дескрипторы намеренно не закрываем; клиентские объекты освободит
     * smb2_destroy_context(). */
    printf("RESULT TEST 21: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_22:
  /* ТЕСТ 22: обязательные статусы [MS-SMB2] 3.3.5.18 не зависят от
   * ограничений VFS. Проверяем отсутствующий открытый объект, файл вместо каталога,
   * разрешённый, но не реализованный класс, неизвестный класс и превышение
   * объявленного MaxTransactSize. */
  printf("\n--- TEST 22: QUERY_DIRECTORY Microsoft status contract ---\n");
  {
    const char *directory = test_directory[0] == '\0' ? "" : test_directory;
    struct windows_compound_state compound;
    struct smb2fh *directory_handle = NULL;
    struct smb2fh *file_handle = NULL;
    struct probe_fh_mirror *file_mirror = NULL;
    smb2_file_id missing_file_id;
    uint32_t missing_status = 0;
    uint32_t file_status = 0;
    uint32_t unsupported_status = 0;
    uint32_t invalid_class_status = 0;
    uint32_t oversized_status = 0;
    int test_ok;

    memset(&compound, 0, sizeof(compound));
    test_ok = run_windows_directory_compound(
                  smb2, directory, &compound, NULL, NULL) == 0 &&
              (uint32_t)compound.status[0] == SMB2_STATUS_SUCCESS;
    if (test_ok) {
      memcpy(missing_file_id, compound.file_id, sizeof(missing_file_id));
      missing_file_id[0] ^= 0x5a;
      test_ok = raw_query_directory_status(
                    smb2, missing_file_id,
                    SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION, 65536,
                    &missing_status) == 0;
    }
    if (test_ok) {
      file_handle = smb2_open(smb2, test_path("query_directory_status.bin"),
                              O_CREAT | O_TRUNC | O_RDWR);
      test_ok = file_handle != NULL;
    }
    if (test_ok) {
      file_mirror = (struct probe_fh_mirror*)file_handle;
      test_ok = raw_query_directory_status(
                    smb2, file_mirror->file_id,
                    SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION, 65536,
                    &file_status) == 0 &&
                raw_query_directory_status(
                    smb2, compound.file_id,
                    SMB2_FILE_DIRECTORY_INFORMATION, 65536,
                    &unsupported_status) == 0 &&
                raw_query_directory_status(
                    smb2, compound.file_id, SMB2_FILE_BASIC_INFORMATION,
                    65536, &invalid_class_status) == 0 &&
                raw_query_directory_status(
                    smb2, compound.file_id,
                    SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION, 65537,
                    &oversized_status) == 0;
    }

    test_ok = test_ok && missing_status == SMB2_STATUS_FILE_CLOSED &&
              file_status == SMB2_STATUS_INVALID_PARAMETER &&
              unsupported_status == SMB2_STATUS_NOT_SUPPORTED &&
              invalid_class_status == SMB2_STATUS_INVALID_INFO_CLASS &&
              oversized_status == SMB2_STATUS_INVALID_PARAMETER;
    printf("  missing=%08x file=%08x unsupported=%08x invalid=%08x "
           "oversized=%08x\n",
           (unsigned)missing_status, (unsigned)file_status,
           (unsigned)unsupported_status, (unsigned)invalid_class_status,
           (unsigned)oversized_status);

    if ((uint32_t)compound.status[0] == SMB2_STATUS_SUCCESS) {
      directory_handle = smb2_fh_from_file_id(smb2, &compound.file_id);
      if (directory_handle == NULL ||
          smb2_close(smb2, directory_handle) != 0) {
        test_ok = 0;
      }
    }
    if (file_handle != NULL) {
      test_ok = smb2_close(smb2, file_handle) == 0 && test_ok;
    }
    printf("RESULT TEST 22: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_23:
  /* ТЕСТ 23: byte-range lock принадлежит конкретному Open. Он обязан
   * блокировать READ/WRITE и другой exclusive lock соседнего соединения,
   * сниматься точным UNLOCK и автоматически исчезать при CLOSE. */
  printf("\n--- TEST 23: byte-range LOCK conflicts across SMB opens ---\n");
  {
    const uint32_t shared_lock = 0x00000001;
    const uint32_t exclusive_lock = 0x00000002;
    const uint32_t unlock = 0x00000004;
    const uint32_t fail_immediately = 0x00000010;
    char path[512];
    struct smb2_context *peer = NULL;
    struct smb2fh *seed = NULL;
    struct smb2fh *owner = NULL;
    struct smb2fh *other = NULL;
    uint8_t data[256];
    uint8_t byte = 0x6d;
    uint32_t lock_status = 0;
    uint32_t read_status = 0;
    uint32_t write_status = 0;
    uint32_t peer_lock_status = 0;
    uint32_t unlock_status = 0;
    uint32_t released_status = 0;
    int test_ok = 1;
    int index;

    snprintf(path, sizeof(path), "%s", test_path("lock_contract.bin"));
    for (index = 0; index < (int)sizeof(data); ++index) {
      data[index] = (uint8_t)index;
    }
    seed = smb2_open(smb2, path, O_CREAT | O_TRUNC | O_RDWR);
    test_ok = seed != NULL;
    if (test_ok) {
      test_ok = smb2_write(smb2, seed, data, sizeof(data)) ==
                (int)sizeof(data);
    }
    if (seed != NULL) {
      test_ok = smb2_close(smb2, seed) == 0 && test_ok;
      seed = NULL;
    }
    if (test_ok) {
      peer = connect_context(server, share);
      test_ok = peer != NULL;
    }
    if (test_ok) {
      owner = smb2_open(smb2, path, O_RDWR);
      other = smb2_open(peer, path, O_RDWR);
      test_ok = owner != NULL && other != NULL;
    }
    if (test_ok) {
      test_ok = raw_lock_status(
                    smb2, owner, 0, 128,
                    exclusive_lock | fail_immediately, &lock_status) == 0 &&
                raw_read_status(peer, other, 0, &byte, 1,
                                &read_status) == 0 &&
                raw_write_status(peer, other, 0, &byte, 1,
                                 &write_status) == 0 &&
                raw_lock_status(
                    peer, other, 0, 128,
                    exclusive_lock | fail_immediately,
                    &peer_lock_status) == 0 &&
                raw_lock_status(smb2, owner, 0, 128, unlock,
                                &unlock_status) == 0;
    }
    if (test_ok) {
      uint32_t allowed_read = 0;
      uint32_t allowed_write = 0;
      test_ok = raw_read_status(peer, other, 0, &byte, 1,
                                &allowed_read) == 0 &&
                raw_write_status(peer, other, 0, &byte, 1,
                                 &allowed_write) == 0 &&
                allowed_read == SMB2_STATUS_SUCCESS &&
                allowed_write == SMB2_STATUS_SUCCESS;
    }
    if (test_ok) {
      /* Shared lock также должен запрещать запись другого Open. */
      uint32_t shared_status = 0;
      uint32_t shared_write_status = 0;
      uint32_t shared_unlock_status = 0;
      test_ok = raw_lock_status(
                    smb2, owner, 128, 64,
                    shared_lock | fail_immediately, &shared_status) == 0 &&
                raw_write_status(peer, other, 128, &byte, 1,
                                 &shared_write_status) == 0 &&
                raw_lock_status(smb2, owner, 128, 64, unlock,
                                &shared_unlock_status) == 0 &&
                shared_status == SMB2_STATUS_SUCCESS &&
                shared_write_status == SMB2_STATUS_FILE_LOCK_CONFLICT &&
                shared_unlock_status == SMB2_STATUS_SUCCESS;
    }
    if (test_ok) {
      test_ok = raw_lock_status(
                    smb2, owner, 192, 32,
                    exclusive_lock | fail_immediately, &released_status) == 0 &&
                released_status == SMB2_STATUS_SUCCESS;
    }
    if (owner != NULL) {
      test_ok = smb2_close(smb2, owner) == 0 && test_ok;
      owner = NULL;
    }
    if (test_ok) {
      uint32_t close_release = 0;
      uint32_t close_unlock = 0;
      test_ok = raw_lock_status(
                    peer, other, 192, 32,
                    exclusive_lock | fail_immediately, &close_release) == 0 &&
                raw_lock_status(peer, other, 192, 32, unlock,
                                &close_unlock) == 0 &&
                close_release == SMB2_STATUS_SUCCESS &&
                close_unlock == SMB2_STATUS_SUCCESS;
    }
    test_ok = test_ok && lock_status == SMB2_STATUS_SUCCESS &&
              read_status == SMB2_STATUS_FILE_LOCK_CONFLICT &&
              write_status == SMB2_STATUS_FILE_LOCK_CONFLICT &&
              peer_lock_status == SMB2_STATUS_LOCK_NOT_GRANTED &&
              unlock_status == SMB2_STATUS_SUCCESS;
    printf("  lock=%08x read=%08x write=%08x peer-lock=%08x "
           "unlock=%08x close-release=%08x\n",
           (unsigned)lock_status, (unsigned)read_status,
           (unsigned)write_status, (unsigned)peer_lock_status,
           (unsigned)unlock_status, (unsigned)released_status);
    if (owner != NULL) smb2_close(smb2, owner);
    if (other != NULL) smb2_close(peer, other);
    if (peer != NULL) {
      smb2_disconnect_share(peer);
      smb2_destroy_context(peer);
    }
    printf("RESULT TEST 23: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_24:
  /* ТЕСТ 24: медленный READ остаётся синхронным на проводе. CANCEL адресуется
   * по MessageId, не расходует credit/номер последовательности и завершает
   * только исходный запрос ответом STATUS_CANCELLED без interim PENDING. */
  printf("\n--- TEST 24: exact synchronous READ CANCEL contract ---\n");
  {
    const uint32_t length = 8192;
    struct smb2fh *writer = NULL;
    struct smb2fh *reader = NULL;
    struct probe_fh_mirror *mirror = NULL;
    struct raw_async_read_state state;
    struct smb2_read_request request;
    struct smb2_pdu *pdu = NULL;
    uint8_t *buffer = (uint8_t*)malloc(length);
    int completed = 0;
    int event_counter = 0;
    int old_passthrough = 0;
    uint64_t sequence_before_cancel = 0;
    uint64_t sequence_after_cancel = 0;
    uint64_t target_message_id = 0;
    int credits_before_cancel = 0;
    int credits_after_cancel = 0;
    int echo_ok = 0;
    int test_ok = buffer != NULL;
    uint32_t index;

    for (index = 0; buffer != NULL && index < length; ++index) {
      buffer[index] = test_byte(index);
    }
    if (test_ok) {
      writer = smb2_open(smb2, test_path("cancel_async.bin"),
                         O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = writer != NULL;
    }
    if (test_ok) {
      test_ok = smb2_write(smb2, writer, buffer, length) == (int)length;
    }
    if (writer != NULL) {
      test_ok = smb2_close(smb2, writer) == 0 && test_ok;
      writer = NULL;
    }
    if (test_ok) {
      reader = smb2_open(smb2, test_path("cancel_async.bin"), O_RDONLY);
      test_ok = reader != NULL;
    }
    memset(&state, 0, sizeof(state));
    memset(&request, 0, sizeof(request));
    if (test_ok) {
      mirror = (struct probe_fh_mirror*)reader;
      state.buffer = buffer;
      state.expected_length = length;
      state.completed_total = &completed;
      state.event_counter = &event_counter;
      state.final_status = -1;
      request.length = length;
      request.buf = buffer;
      memcpy(request.file_id, mirror->file_id, sizeof(request.file_id));
      smb2_get_passthrough(smb2, &old_passthrough);
      smb2_set_passthrough(smb2, 1);
      target_message_id = smb2->message_id;
      pdu = smb2_cmd_read_async(smb2, &request, raw_async_read_cb, &state);
      test_ok = pdu != NULL;
    }
    if (test_ok) {
      smb2_queue_pdu(smb2, pdu);
      sequence_before_cancel = smb2->message_id;
      credits_before_cancel = smb2->credits;
      /* Без interim AsyncId нет: Windows адресует CANCEL по исходному MID. */
      pdu = smb2_cmd_cancel_async(smb2, target_message_id, 0);
      test_ok = pdu != NULL;
      if (pdu != NULL) smb2_queue_pdu(smb2, pdu);
    }
    if (test_ok) {
      test_ok = wait_for_count(smb2, &completed, 1, 10000) == 0;
    }
    sequence_after_cancel = smb2->message_id;
    credits_after_cancel = smb2->credits;
    test_ok = test_ok && state.pending_count == 0 &&
              state.final_count == 1 &&
              (uint32_t)state.final_status == SMB2_STATUS_CANCELLED &&
              state.corrupted == -1 &&
              state.final_message_id == target_message_id &&
              (state.final_flags & SMB2_FLAGS_SERVER_TO_REDIR) != 0 &&
              (state.final_flags & SMB2_FLAGS_ASYNC_COMMAND) == 0 &&
              state.final_credit != 0 &&
              sequence_after_cancel == sequence_before_cancel &&
              credits_after_cancel == credits_before_cancel + 1;
    echo_ok = smb2_echo(smb2) == 0;
    test_ok = test_ok && echo_ok;
    smb2_set_passthrough(smb2, old_passthrough);
    printf("  pending=%d target-mid=0x%016llx final=%08x/0x%016llx "
           "credit=%u sequence=%llu/%llu credits=%d/%d\n",
           state.pending_count, (unsigned long long)target_message_id,
           (unsigned)state.final_status,
           (unsigned long long)state.final_message_id,
           (unsigned)state.final_credit,
           (unsigned long long)sequence_before_cancel,
           (unsigned long long)sequence_after_cancel,
           credits_before_cancel, credits_after_cancel);
    if (reader != NULL) test_ok = smb2_close(smb2, reader) == 0 && test_ok;
    if (writer != NULL) smb2_close(smb2, writer);
    free(buffer);
    printf("RESULT TEST 24: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_25:
  /* ТЕСТ 25 повторяет фактическую строку O22 Samba: отдельный async
   * STATUS_PENDING, FILE_ACTION_ADDED после CREATE и STATUS_CANCELLED после
   * точного CANCEL по AsyncId. */
  printf("\n--- TEST 25: Samba-style asynchronous CHANGE_NOTIFY ---\n");
  {
    struct windows_compound_state compound;
    struct smb2fh *directory_handle = NULL;
    struct smb2fh *created = NULL;
    struct raw_change_notify_state event_state;
    struct raw_change_notify_state rename_state;
    struct raw_change_notify_state cancel_state;
    struct smb2_pdu *cancel_pdu = NULL;
    char event_path[512];
    char renamed_path[512];
    int event_completed = 0;
    int rename_completed = 0;
    int cancel_completed = 0;
    int old_passthrough = 0;
    int test_ok;
    memset(&compound, 0, sizeof(compound));
    memset(&event_state, 0, sizeof(event_state));
    memset(&rename_state, 0, sizeof(rename_state));
    memset(&cancel_state, 0, sizeof(cancel_state));
    snprintf(event_path, sizeof(event_path), "%s", test_path("notify_contract.txt"));
    snprintf(renamed_path, sizeof(renamed_path), "%s",
             test_path("notify_renamed.txt"));
    smb2_get_passthrough(smb2, &old_passthrough);
    smb2_set_passthrough(smb2, 1);
    /* Остаток прерванного предыдущего запуска не должен превратить CREATE в
     * overwrite и изменить тип ожидаемого уведомления. */
    (void)smb2_unlink(smb2, event_path);
    (void)smb2_unlink(smb2, renamed_path);
    test_ok = run_windows_directory_compound(
                  smb2, test_directory[0] == '\0' ? "" : test_directory,
                  &compound, NULL, NULL) == 0 &&
              (uint32_t)compound.status[0] == SMB2_STATUS_SUCCESS;
    if (test_ok) {
      test_ok = queue_raw_change_notify(smb2, compound.file_id, &event_state,
                                        &event_completed) == 0 &&
                wait_for_count(smb2, &event_state.pending_count, 1, 10000) == 0;
    }
    if (test_ok) {
      created = smb2_open(smb2, event_path, O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = created != NULL;
    }
    if (created != NULL) {
      static const uint8_t payload[] = "notify";
      test_ok = smb2_write(smb2, created, payload,
                           (uint32_t)(sizeof(payload) - 1)) ==
                    (int)(sizeof(payload) - 1) &&
                smb2_close(smb2, created) == 0 && test_ok;
      created = NULL;
    }
    if (test_ok && event_completed == 0) {
      test_ok = wait_for_count(smb2, &event_completed, 1, 10000) == 0;
    }
    test_ok = test_ok && event_state.pending_count == 1 &&
              event_state.final_count == 1 &&
              (uint32_t)event_state.final_status == SMB2_STATUS_SUCCESS &&
              event_state.payload_valid &&
              event_state.entry_count == 1 &&
              event_state.action == SMB2_NOTIFY_CHANGE_FILE_ACTION_ADDED &&
              strcmp(event_state.name, "notify_contract.txt") == 0 &&
              event_state.pending_command == SMB2_CHANGE_NOTIFY &&
              event_state.final_command == SMB2_CHANGE_NOTIFY &&
              (event_state.pending_flags &
                   (SMB2_FLAGS_SERVER_TO_REDIR | SMB2_FLAGS_ASYNC_COMMAND)) ==
                  (SMB2_FLAGS_SERVER_TO_REDIR | SMB2_FLAGS_ASYNC_COMMAND) &&
              (event_state.final_flags &
                   (SMB2_FLAGS_SERVER_TO_REDIR | SMB2_FLAGS_ASYNC_COMMAND)) ==
                  (SMB2_FLAGS_SERVER_TO_REDIR | SMB2_FLAGS_ASYNC_COMMAND) &&
              event_state.pending_credit != 0 && event_state.final_credit == 0 &&
              event_state.pending_message_id == event_state.final_message_id &&
              event_state.pending_async_id != 0 &&
              event_state.pending_async_id == event_state.final_async_id;

    if (test_ok) {
      test_ok = queue_raw_change_notify(smb2, compound.file_id, &rename_state,
                                        &rename_completed) == 0 &&
                wait_for_count(smb2, &rename_state.pending_count, 1, 10000) == 0;
    }
    if (test_ok) {
      /* Passthrough нужен клиенту только для callback промежуточного PENDING.
       * Типизированный smb2_rename должен кодировать FILE_RENAME_INFORMATION
       * штатно, поэтому после PENDING временно возвращаем обычный режим. */
      smb2_set_passthrough(smb2, old_passthrough);
      test_ok = smb2_rename(smb2, event_path, renamed_path) == 0;
    }
    if (test_ok && rename_completed == 0) {
      test_ok = wait_for_count(smb2, &rename_completed, 1, 10000) == 0;
    }
    test_ok = test_ok && rename_state.pending_count == 1 &&
              rename_state.final_count == 1 &&
              (uint32_t)rename_state.final_status == SMB2_STATUS_SUCCESS &&
              rename_state.payload_valid && rename_state.entry_count == 2 &&
              rename_state.action ==
                  SMB2_NOTIFY_CHANGE_FILE_ACTION_RENAMED_OLD_NAME &&
              strcmp(rename_state.name, "notify_contract.txt") == 0 &&
              rename_state.second_action ==
                  SMB2_NOTIFY_CHANGE_FILE_ACTION_RENAMED_NEW_NAME &&
              strcmp(rename_state.second_name, "notify_renamed.txt") == 0 &&
              rename_state.pending_message_id == rename_state.final_message_id &&
              rename_state.pending_async_id != 0 &&
              rename_state.pending_async_id == rename_state.final_async_id;

    smb2_set_passthrough(smb2, 1);
    if (test_ok) {
      test_ok = queue_raw_change_notify(smb2, compound.file_id, &cancel_state,
                                        &cancel_completed) == 0 &&
                wait_for_count(smb2, &cancel_state.pending_count, 1, 10000) == 0;
    }
    if (test_ok) {
      cancel_pdu = smb2_cmd_cancel_async(smb2, 0,
                                         cancel_state.pending_async_id);
      test_ok = cancel_pdu != NULL;
      if (cancel_pdu != NULL) smb2_queue_pdu(smb2, cancel_pdu);
    }
    if (test_ok) {
      test_ok = wait_for_count(smb2, &cancel_completed, 1, 10000) == 0;
    }
    test_ok = test_ok && cancel_state.pending_count == 1 &&
              cancel_state.final_count == 1 &&
              (uint32_t)cancel_state.final_status == SMB2_STATUS_CANCELLED &&
              cancel_state.pending_message_id == cancel_state.final_message_id &&
              cancel_state.pending_async_id != 0 &&
              cancel_state.pending_async_id == cancel_state.final_async_id;

    if ((uint32_t)compound.status[0] == SMB2_STATUS_SUCCESS) {
      directory_handle = smb2_fh_from_file_id(smb2, &compound.file_id);
      test_ok = directory_handle != NULL &&
                smb2_close(smb2, directory_handle) == 0 && test_ok;
    }
    (void)smb2_unlink(smb2, event_path);
    (void)smb2_unlink(smb2, renamed_path);
    smb2_set_passthrough(smb2, old_passthrough);
    printf("  event pending=%d final=%08x action=%u name=%s async=0x%016llx\n",
           event_state.pending_count, (unsigned)event_state.final_status,
           (unsigned)event_state.action, event_state.name,
           (unsigned long long)event_state.pending_async_id);
    printf("  rename entries=%d %u:%s -> %u:%s final=%08x\n",
           rename_state.entry_count, (unsigned)rename_state.action,
           rename_state.name, (unsigned)rename_state.second_action,
           rename_state.second_name, (unsigned)rename_state.final_status);
    printf("  cancel pending=%d final=%08x async=0x%016llx\n",
           cancel_state.pending_count, (unsigned)cancel_state.final_status,
           (unsigned long long)cancel_state.pending_async_id);
    printf("RESULT TEST 25: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_26:
  /* ТЕСТ 26: LOGOFF освобождает только свой сеанс. Ответ обязан прийти, а
   * параллельный основной сеанс после этого продолжает отвечать на ECHO. */
  printf("\n--- TEST 26: LOGOFF response and session isolation ---\n");
  {
    struct smb2_context *peer = connect_context(server, share);
    struct raw_command_state state;
    struct smb2_pdu *pdu = NULL;
    int test_ok = peer != NULL;
    memset(&state, 0, sizeof(state));
    state.status = -1;
    if (test_ok) {
      pdu = smb2_cmd_logoff_async(peer, raw_command_cb, &state);
      test_ok = pdu != NULL;
    }
    if (test_ok) {
      smb2_queue_pdu(peer, pdu);
      test_ok = wait_raw_command(peer, &state) == 0 &&
                (uint32_t)state.status == SMB2_STATUS_SUCCESS;
    }
    if (peer != NULL) {
      /* После успешного LOGOFF TREE_DISCONNECT/повторный LOGOFF уже не шлём. */
      smb2_destroy_context(peer);
    }
    test_ok = smb2_echo(smb2) == 0 && test_ok;
    printf("  LOGOFF status=%08x main-ECHO=%s\n",
           (unsigned)state.status, test_ok ? "PASS" : "FAIL");
    printf("RESULT TEST 26: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_27:
  /* ТЕСТ 27: чтение метаданных не является записью на EVO и не имеет права
   * сбрасывать курсор каталога. Первый полный физический проход должен создать
   * снимок, который переживает закрытие TCP-сеанса; следующее подключение
   * получает пакет записей из PSRAM без нового FINDNEXT. */
  printf("\n--- TEST 27: read-only STAT keeps cursor and cache survives reconnect ---\n");
  {
    enum { seed_count = 20 };
    const char *directory = test_directory[0] == '\0' ? "" : test_directory;
    struct windows_compound_state cold;
    struct windows_compound_state cached;
    struct raw_directory_state next;
    struct smb2_context *peer = NULL;
    struct smb2fh *directory_handle = NULL;
    struct smb2fh *cached_handle = NULL;
    struct smb2fh *seed = NULL;
    struct smb2_stat_64 stat_result;
    char marker_path[512];
    uint32_t last_index = 0;
    int cursor_ok = 0;
    int enumeration_ok = 0;
    int cache_ok = 0;
    int iterations = 0;
    DWORD cached_elapsed_ms = 0;
    int test_ok = 1;
    int index;

    memset(&cold, 0, sizeof(cold));
    memset(&cached, 0, sizeof(cached));
    memset(&next, 0, sizeof(next));
    memset(&stat_result, 0, sizeof(stat_result));
    snprintf(marker_path, sizeof(marker_path), "%s",
             test_path("cache_cursor_seed_00.tmp"));

    /* Сначала намеренно меняем EVO. После последнего CREATE начинается
     * проверяемый интервал, в котором нет ни одной операции записи. */
    for (index = 0; test_ok && index < seed_count; ++index) {
      char name[64];
      snprintf(name, sizeof(name), "cache_cursor_seed_%02d.tmp", index);
      seed = smb2_open(smb2, test_path(name),
                       O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = seed != NULL;
      if (seed != NULL) {
        test_ok = smb2_close(smb2, seed) == 0 && test_ok;
        seed = NULL;
      }
    }

    if (test_ok) {
      test_ok = run_windows_directory_compound(
                    smb2, directory, &cold, NULL, NULL) == 0 &&
                (uint32_t)cold.status[0] == SMB2_STATUS_SUCCESS &&
                (uint32_t)cold.status[1] == SMB2_STATUS_SUCCESS &&
                (uint32_t)cold.status[2] == SMB2_STATUS_SUCCESS &&
                cold.directory_payload_valid[0] &&
                cold.directory_payload_valid[1] &&
                cold.directory_entry_count[0] == 1 &&
                cold.directory_entry_count[1] == 1 &&
                cold.directory_first_index[0] > 0 &&
                cold.directory_first_index[1] ==
                    cold.directory_first_index[0] + 1;
      last_index = cold.directory_first_index[1];
    }

    /* Именно этот read-only STAT раньше вызывал invalidateParent(), обнулял
     * directoryIndex открытого handle и возвращал первую запись повторно. */
    if (test_ok) {
      test_ok = smb2_stat(smb2, marker_path, &stat_result) == 0;
    }
    if (test_ok) {
      test_ok = raw_query_directory_entries(
                    smb2, cold.file_id, 0, &next) == 0 &&
                (uint32_t)next.status == SMB2_STATUS_SUCCESS &&
                next.payload_valid && next.padding_zero &&
                next.entry_count == 1;
      cursor_ok = test_ok && next.first_index == last_index + 1;
      test_ok = test_ok && cursor_ok;
      last_index = next.first_index;
    }

    /* Доходим до STATUS_NO_MORE_FILES: только в этот момент снимок становится
     * полным и может быть опубликован для других подключений. */
    while (test_ok && iterations++ < 4096) {
      if (raw_query_directory_entries(smb2, cold.file_id, 0, &next) != 0) {
        test_ok = 0;
        break;
      }
      if ((uint32_t)next.status == SMB2_STATUS_NO_MORE_FILES) {
        enumeration_ok = 1;
        break;
      }
      if ((uint32_t)next.status != SMB2_STATUS_SUCCESS ||
          !next.payload_valid || !next.padding_zero ||
          next.entry_count != 1 || next.first_index != last_index + 1) {
        test_ok = 0;
        break;
      }
      last_index = next.first_index;
    }
    test_ok = test_ok && enumeration_ok;

    if ((uint32_t)cold.status[0] == SMB2_STATUS_SUCCESS) {
      directory_handle = smb2_fh_from_file_id(smb2, &cold.file_id);
      test_ok = directory_handle != NULL &&
                smb2_close(smb2, directory_handle) == 0 && test_ok;
      directory_handle = NULL;
    }

    if (test_ok) {
      peer = connect_context(server, share);
      test_ok = peer != NULL;
    }
    if (test_ok) {
      const DWORD started = GetTickCount();
      test_ok = run_windows_directory_compound(
                    peer, directory, &cached, NULL, NULL) == 0;
      cached_elapsed_ms = GetTickCount() - started;
      cache_ok = test_ok &&
                 (uint32_t)cached.status[0] == SMB2_STATUS_SUCCESS &&
                 (uint32_t)cached.status[1] == SMB2_STATUS_SUCCESS &&
                 cached.directory_payload_valid[0] &&
                 cached.directory_entry_count[0] > 1 &&
                 cached_elapsed_ms < 300;
      test_ok = test_ok && cache_ok;
    }
    if (peer != NULL &&
        (uint32_t)cached.status[0] == SMB2_STATUS_SUCCESS) {
      cached_handle = smb2_fh_from_file_id(peer, &cached.file_id);
      test_ok = cached_handle != NULL &&
                smb2_close(peer, cached_handle) == 0 && test_ok;
      cached_handle = NULL;
    }

    printf("  Cold indices=%lu,%lu then STAT -> %lu (%s)\n",
           (unsigned long)cold.directory_first_index[0],
           (unsigned long)cold.directory_first_index[1],
           (unsigned long)(cursor_ok ? cold.directory_first_index[1] + 1 :
                           next.first_index),
           cursor_ok ? "PASS" : "FAIL");
    printf("  Full enumeration=%s last-index=%lu requests=%d\n",
           enumeration_ok ? "PASS" : "FAIL", (unsigned long)last_index,
           iterations);
    printf("  Reconnect cached first-batch=%d elapsed=%lu ms (%s)\n",
           cached.directory_entry_count[0],
           (unsigned long)cached_elapsed_ms, cache_ok ? "PASS" : "FAIL");

    if (peer != NULL) {
      smb2_disconnect_share(peer);
      smb2_destroy_context(peer);
    }
    /* Уборка идёт уже после результата и потому не участвует в проверяемом
     * read-only интервале. */
    for (index = 0; index < seed_count; ++index) {
      char name[64];
      snprintf(name, sizeof(name), "cache_cursor_seed_%02d.tmp", index);
      (void)smb2_unlink(smb2, test_path(name));
    }
    printf("RESULT TEST 27: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_28:
  /* ТЕСТ 28: два холодных QUERY_DIRECTORY поставлены раньше READ. Первый QD
   * уже занимает физический FILEX, второй ждёт в очереди, а затем приходит
   * чтение файла. Более поздний READ не имеет права обогнать второй QD — это
   * точная регрессия зависания каталога из трассы Проводника. */
  printf("\n--- TEST 28: global FIFO keeps older directory ahead of later READ ---\n");
  {
    enum { seed_count = 10 };
    const uint32_t read_length = 32768;
    const char *directory = test_directory[0] == '\0' ? "" : test_directory;
    struct windows_compound_state opened;
    struct raw_directory_state first;
    struct raw_directory_state second;
    struct raw_async_read_state read_state;
    struct smb2_query_directory_request first_request;
    struct smb2_query_directory_request second_request;
    struct smb2_read_request read_request;
    struct smb2_pdu *first_pdu = NULL;
    struct smb2_pdu *second_pdu = NULL;
    struct smb2_pdu *read_pdu = NULL;
    struct smb2fh *writer = NULL;
    struct smb2fh *reader = NULL;
    struct smb2fh *directory_handle = NULL;
    struct probe_fh_mirror *read_mirror = NULL;
    uint8_t *write_buffer = (uint8_t*)malloc(read_length);
    uint8_t *read_buffer = (uint8_t*)calloc(read_length, 1);
    int completed = 0;
    int event_counter = 0;
    int old_passthrough = 0;
    int test_ok = write_buffer != NULL && read_buffer != NULL;
    int index;

    memset(&opened, 0, sizeof(opened));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&read_state, 0, sizeof(read_state));
    memset(&first_request, 0, sizeof(first_request));
    memset(&second_request, 0, sizeof(second_request));
    memset(&read_request, 0, sizeof(read_request));
    for (index = 0; write_buffer != NULL && index < (int)read_length; ++index) {
      write_buffer[index] = test_byte((uint64_t)index);
    }

    if (test_ok) {
      writer = smb2_open(smb2, test_path("fifo_later_read.bin"),
                         O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = writer != NULL;
    }
    if (test_ok) {
      test_ok = smb2_write(smb2, writer, write_buffer, read_length) ==
                (int)read_length;
    }
    if (writer != NULL) {
      test_ok = smb2_close(smb2, writer) == 0 && test_ok;
      writer = NULL;
    }
    for (index = 0; test_ok && index < seed_count; ++index) {
      char name[64];
      struct smb2fh *seed;
      snprintf(name, sizeof(name), "fifo_directory_seed_%02d.tmp", index);
      seed = smb2_open(smb2, test_path(name), O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = seed != NULL;
      if (seed != NULL) {
        test_ok = smb2_close(smb2, seed) == 0 && test_ok;
      }
    }
    if (test_ok) {
      reader = smb2_open(smb2, test_path("fifo_later_read.bin"), O_RDONLY);
      test_ok = reader != NULL;
    }
    if (test_ok) {
      test_ok = run_windows_directory_compound(
                    smb2, directory, &opened, NULL, NULL) == 0 &&
                (uint32_t)opened.status[0] == SMB2_STATUS_SUCCESS &&
                (uint32_t)opened.status[1] == SMB2_STATUS_SUCCESS &&
                (uint32_t)opened.status[2] == SMB2_STATUS_SUCCESS;
    }

    if (test_ok) {
      first.completed_total = &completed;
      first.event_counter = &event_counter;
      first.status = -1;
      second.completed_total = &completed;
      second.event_counter = &event_counter;
      second.status = -1;
      read_state.buffer = read_buffer;
      read_state.expected_length = read_length;
      read_state.offset = 0;
      read_state.completed_total = &completed;
      read_state.event_counter = &event_counter;
      read_state.final_status = -1;

      first_request.file_information_class =
          SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION;
      first_request.output_buffer_length = 65536;
      first_request.name = "*";
      memcpy(first_request.file_id, opened.file_id,
             sizeof(first_request.file_id));
      second_request = first_request;

      read_mirror = (struct probe_fh_mirror*)reader;
      read_request.length = read_length;
      read_request.buf = read_buffer;
      memcpy(read_request.file_id, read_mirror->file_id,
             sizeof(read_request.file_id));

      smb2_get_passthrough(smb2, &old_passthrough);
      smb2_set_passthrough(smb2, 1);
      first_pdu = smb2_cmd_query_directory_async(
          smb2, &first_request, raw_directory_cb, &first);
      second_pdu = smb2_cmd_query_directory_async(
          smb2, &second_request, raw_directory_cb, &second);
      read_pdu = smb2_cmd_read_async(
          smb2, &read_request, raw_async_read_cb, &read_state);
      test_ok = first_pdu != NULL && second_pdu != NULL && read_pdu != NULL;
    }
    if (test_ok) {
      smb2_queue_pdu(smb2, first_pdu);
      smb2_queue_pdu(smb2, second_pdu);
      smb2_queue_pdu(smb2, read_pdu);
      test_ok = wait_for_count(smb2, &completed, 3, 30000) == 0;
    }

    test_ok = test_ok && first.done && second.done && read_state.completed &&
              (uint32_t)first.status == SMB2_STATUS_SUCCESS &&
              (uint32_t)second.status == SMB2_STATUS_SUCCESS &&
              first.payload_valid && second.payload_valid &&
              first.padding_zero && second.padding_zero &&
              first.entry_count == 1 && second.entry_count == 1 &&
              first.first_index == opened.directory_first_index[1] + 1 &&
              second.first_index == first.first_index + 1 &&
              read_state.pending_count == 0 && read_state.final_count == 1 &&
              (uint32_t)read_state.final_status == SMB2_STATUS_SUCCESS &&
              read_state.corrupted == 0 && first.final_order > 0 &&
              first.final_order < second.final_order &&
              second.final_order < read_state.final_order;

    printf("  Directory indices=%lu,%lu orders=%d,%d\n",
           (unsigned long)first.first_index,
           (unsigned long)second.first_index,
           first.final_order, second.final_order);
    printf("  Later READ callbacks=%d/%d orders=%d/%d bytes=%lu bad=%d\n",
           read_state.pending_count, read_state.final_count,
           read_state.pending_order, read_state.final_order,
           (unsigned long)read_state.expected_length, read_state.corrupted);
    printf("  FIFO QD1 < QD2 < READ: %s\n", test_ok ? "PASS" : "FAIL");

    smb2_set_passthrough(smb2, old_passthrough);
    if (reader != NULL) {
      test_ok = smb2_close(smb2, reader) == 0 && test_ok;
    }
    if ((uint32_t)opened.status[0] == SMB2_STATUS_SUCCESS) {
      directory_handle = smb2_fh_from_file_id(smb2, &opened.file_id);
      test_ok = directory_handle != NULL &&
                smb2_close(smb2, directory_handle) == 0 && test_ok;
    }
    (void)smb2_unlink(smb2, test_path("fifo_later_read.bin"));
    for (index = 0; index < seed_count; ++index) {
      char name[64];
      snprintf(name, sizeof(name), "fifo_directory_seed_%02d.tmp", index);
      (void)smb2_unlink(smb2, test_path(name));
    }
    free(read_buffer);
    free(write_buffer);
    printf("RESULT TEST 28: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_29:
  /* ТЕСТ 29 повторяет наблюдателя Проводника из реальной трассы копирования.
   * Пишущий Open заранее задаёт конечный EOF, после чего другой SMB-сеанс
   * открывает тот же ещё недописанный файл только для чтения и закрывает его.
   * Наблюдатель обязан видеть логический EOF, но не владеет резервом и поэтому
   * его CLOSE не имеет права физически расширять файл на EVO. */
  printf("\n--- TEST 29: read-only CLOSE does not commit another Open's EOF ---\n");
  {
    const uint64_t reserved_eof = 3182298;
    struct smb2_context *observer_context = NULL;
    struct smb2fh *owner = NULL;
    struct smb2fh *observer = NULL;
    uint64_t owner_allocation_before = 0;
    uint64_t owner_eof_before = 0;
    uint64_t observer_allocation = 0;
    uint64_t observer_eof = 0;
    uint64_t owner_allocation_after = 0;
    uint64_t owner_eof_after = 0;
    int observer_close = -1;
    int cleanup_truncate = -1;
    int owner_close = -1;
    int test_ok = 1;

    owner = smb2_open(smb2, test_path("observer_reserved_eof.bin"),
                      O_CREAT | O_TRUNC | O_WRONLY);
    test_ok = owner != NULL;
    if (test_ok) {
      test_ok = smb2_ftruncate(smb2, owner, reserved_eof) == 0;
    }
    if (test_ok) {
      test_ok = query_file_allocation(
                    smb2, owner, SMB2_FILE_STANDARD_INFORMATION,
                    &owner_allocation_before, &owner_eof_before) == 0 &&
                owner_eof_before == reserved_eof;
    }
    if (test_ok) {
      observer_context = connect_context(server, share);
      test_ok = observer_context != NULL;
    }
    if (test_ok) {
      observer = smb2_open(observer_context,
                           test_path("observer_reserved_eof.bin"), O_RDONLY);
      test_ok = observer != NULL;
    }
    if (test_ok) {
      test_ok = query_file_allocation(
                    observer_context, observer,
                    SMB2_FILE_STANDARD_INFORMATION, &observer_allocation,
                    &observer_eof) == 0 &&
                observer_eof == reserved_eof &&
                observer_allocation == owner_allocation_before;
    }
    if (observer != NULL) {
      observer_close = smb2_close(observer_context, observer);
      observer = NULL;
      test_ok = observer_close == 0 && test_ok;
    }
    if (test_ok) {
      test_ok = query_file_allocation(
                    smb2, owner, SMB2_FILE_STANDARD_INFORMATION,
                    &owner_allocation_after, &owner_eof_after) == 0 &&
                owner_eof_after == reserved_eof &&
                owner_allocation_after == owner_allocation_before;
    }

    printf("  Owner before: EOF=%llu AllocationSize=%llu\n",
           (unsigned long long)owner_eof_before,
           (unsigned long long)owner_allocation_before);
    printf("  Observer: EOF=%llu AllocationSize=%llu CLOSE=%d\n",
           (unsigned long long)observer_eof,
           (unsigned long long)observer_allocation, observer_close);
    printf("  Owner after observer CLOSE: EOF=%llu AllocationSize=%llu\n",
           (unsigned long long)owner_eof_after,
           (unsigned long long)owner_allocation_after);

    if (owner != NULL) {
      /* Тесту не требуется материализовать трёхмегабайтный хвост. Снимаем
       * резерв штатным SET_EOF=0 и только затем закрываем владельца. */
      cleanup_truncate = smb2_ftruncate(smb2, owner, 0);
      test_ok = cleanup_truncate == 0 && test_ok;
      owner_close = smb2_close(smb2, owner);
      owner = NULL;
      test_ok = owner_close == 0 && test_ok;
    }
    if (observer_context != NULL) {
      smb2_disconnect_share(observer_context);
      smb2_destroy_context(observer_context);
    }
    (void)smb2_unlink(smb2, test_path("observer_reserved_eof.bin"));
    printf("  Cleanup SET_EOF=0=%d Owner CLOSE=%d\n",
           cleanup_truncate, owner_close);
    printf("RESULT TEST 29: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_30:
  /* ТЕСТ 30 воспроизводит фактическую гонку Проводника из packet capture:
   * холодный каталог вернул одну запись, следующий FINDNEXT уже занял FILEX,
   * а другой SMB-сеанс открывает только что показанный файл. CREATE обязан
   * использовать сохранённый результат перечисления, а не отвечать
   * STATUS_IO_TIMEOUT из-за занятого физического канала. Сервер стенда нужно
   * запускать с ненулевой задержкой каталога (рекомендуется 250 мс). */
  printf("\n--- TEST 30: CREATE of last cold entry while next FINDNEXT is active ---\n");
  {
    enum { seed_count = 12 };
    const char *directory = test_directory[0] == '\0' ? "" : test_directory;
    struct windows_compound_state opened;
    struct raw_directory_state current;
    struct raw_directory_state next;
    struct smb2_context *peer = NULL;
    struct smb2fh *seed = NULL;
    struct smb2fh *opened_entry = NULL;
    struct smb2fh *directory_handle = NULL;
    char selected_name[256] = {0};
    char selected_path[512] = {0};
    int found = 0;
    int queued = 0;
    int next_completed = 0;
    DWORD create_elapsed_ms = 0;
    int test_ok = 1;
    int index;

    memset(&opened, 0, sizeof(opened));
    memset(&current, 0, sizeof(current));
    memset(&next, 0, sizeof(next));

    for (index = 0; test_ok && index < seed_count; ++index) {
      char name[64];
      snprintf(name, sizeof(name), "zz_overlap_seed_%02d.tmp", index);
      seed = smb2_open(smb2, test_path(name), O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = seed != NULL;
      if (seed != NULL) {
        test_ok = smb2_close(smb2, seed) == 0 && test_ok;
        seed = NULL;
      }
    }

    if (test_ok) {
      peer = connect_context(server, share);
      test_ok = peer != NULL;
    }
    if (test_ok) {
      test_ok = run_windows_directory_compound(
                    smb2, directory, &opened, NULL, NULL) == 0 &&
                (uint32_t)opened.status[0] == SMB2_STATUS_SUCCESS;
    }

    /* Доходим до любой нашей записи. Каждый холодный ответ содержит ровно
     * один элемент, поэтому он становится последней подтверждённой записью
     * открытого каталога. */
    while (test_ok && !found) {
      if (raw_query_directory_entries(smb2, opened.file_id, 0, &current) != 0 ||
          (uint32_t)current.status != SMB2_STATUS_SUCCESS ||
          !current.payload_valid) {
        test_ok = 0;
        break;
      }
      if (strncmp(current.first_name, "zz_overlap_seed_", 16) == 0) {
        snprintf(selected_name, sizeof(selected_name), "%s",
                 current.first_name);
        snprintf(selected_path, sizeof(selected_path), "%s",
                 test_path(selected_name));
        found = 1;
      }
    }
    test_ok = test_ok && found;

    if (test_ok) {
      queued = queue_raw_query_directory_entries(
                   smb2, opened.file_id, 0, &next) == 0;
      test_ok = queued;
    }
    if (test_ok) {
      /* Передаём следующий QUERY_DIRECTORY серверу, но не ждём его финала.
       * При directory-delay=250 физический FINDNEXT гарантированно остаётся
       * активным, когда параллельный сеанс посылает CREATE. */
      test_ok = service_for(smb2, 25) == 0;
    }
    if (test_ok) {
      const DWORD create_started = GetTickCount();
      opened_entry = smb2_open(peer, selected_path, O_RDONLY);
      create_elapsed_ms = GetTickCount() - create_started;
      test_ok = opened_entry != NULL && create_elapsed_ms < 150;
    }
    if (queued) {
      next_completed = wait_for_count(smb2, &next.done, 1, 10000) == 0;
      test_ok = next_completed && test_ok;
    }

    printf("  Last entry=%s parallel CREATE=%s in %lu ms next-QD=%s status=%08x\n",
           selected_name[0] == '\0' ? "(none)" : selected_name,
           opened_entry != NULL ? "SUCCESS" : "FAIL",
           (unsigned long)create_elapsed_ms,
           next_completed ? "completed" : "failed",
           (unsigned)next.status);

    if (opened_entry != NULL) {
      test_ok = smb2_close(peer, opened_entry) == 0 && test_ok;
      opened_entry = NULL;
    }
    if ((uint32_t)opened.status[0] == SMB2_STATUS_SUCCESS) {
      directory_handle = smb2_fh_from_file_id(smb2, &opened.file_id);
      if (directory_handle != NULL) {
        test_ok = smb2_close(smb2, directory_handle) == 0 && test_ok;
        directory_handle = NULL;
      }
    }
    if (peer != NULL) {
      smb2_disconnect_share(peer);
      smb2_destroy_context(peer);
    }
    for (index = 0; index < seed_count; ++index) {
      char name[64];
      snprintf(name, sizeof(name), "zz_overlap_seed_%02d.tmp", index);
      (void)smb2_unlink(smb2, test_path(name));
    }
    printf("RESULT TEST 30: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_31:
  /* Exact NetrShareEnum request from Windows Explorer. ResumeHandle is a
   * NULL [unique] pointer. The response must preserve NULL; turning it into
   * a pointer-to-zero makes Explorer reject the otherwise valid share list
   * and enumerate srvsvc repeatedly. */
  printf("\n--- TEST 31: NetrShareEnum preserves NULL ResumeHandle ---\n");
  {
    static const uint8_t windows_request_null[] = {
      0x05, 0x00, 0x00, 0x03, 0x10, 0x00, 0x00, 0x00,
      0x58, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
      0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00,
      0x00, 0x00, 0x02, 0x00, 0x09, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
      0x5c, 0x00, 0x5c, 0x00, 0x5a, 0x00, 0x58, 0x00,
      0x2d, 0x00, 0x45, 0x00, 0x76, 0x00, 0x6f, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x02, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t windows_request_present[sizeof(windows_request_null) + 4];
    uint8_t response[512];
    struct smb2_dcerpc_request_info info;
    uint16_t presentation_context = 0;
    size_t response_length = 0;
    int result;
    int null_ok;
    int present_ok;

    memset(&info, 0, sizeof(info));
    result = smb2_dcerpc_srvsvc_reply(
        smb2, windows_request_null, sizeof(windows_request_null), "SD",
        "ZX-Evo", &presentation_context, response, sizeof(response),
        &response_length, &info);
    null_ok = result == 0 && response_length == 208 && response[2] == 2 &&
              test_read_le16(response + 8) == response_length &&
              test_read_le32(response + 16) == response_length - 24 &&
              test_read_le32(response + response_length - 12) == 2 &&
              test_read_le32(response + response_length - 8) == 0 &&
              test_read_le32(response + response_length - 4) == 0;

    memcpy(windows_request_present, windows_request_null,
           sizeof(windows_request_null));
    memset(windows_request_present + sizeof(windows_request_null), 0, 4);
    test_write_le16(windows_request_present + 8,
                    (uint16_t)sizeof(windows_request_present));
    test_write_le32(windows_request_present + 16,
                    (uint32_t)sizeof(windows_request_present) - 24);
    test_write_le32(windows_request_present + 84, 0x00020008);
    presentation_context = 0;
    response_length = 0;
    memset(&info, 0, sizeof(info));
    result = smb2_dcerpc_srvsvc_reply(
        smb2, windows_request_present, sizeof(windows_request_present), "SD",
        "ZX-Evo", &presentation_context, response, sizeof(response),
        &response_length, &info);
    present_ok = result == 0 && response_length == 212 && response[2] == 2 &&
                 test_read_le32(response + response_length - 16) == 2 &&
                 test_read_le32(response + response_length - 12) != 0 &&
                 test_read_le32(response + response_length - 8) == 0 &&
                 test_read_le32(response + response_length - 4) == 0;

    printf("  NULL response=%lu bytes pointer=%08x; present response=%lu "
           "bytes pointer=%08x\n",
           (unsigned long)(null_ok ? 208 : response_length),
           null_ok ? 0u : 0xffffffffu,
           (unsigned long)response_length,
           response_length >= 12
               ? (unsigned)test_read_le32(response + response_length - 12)
               : 0u);
    printf("RESULT TEST 31: %s\n",
           null_ok && present_ok ? "PASS" : "FAIL");
    failures += !(null_ok && present_ok);
  }

  if (only_test == NULL) goto done;

test_32:
  /* ТЕСТ 32 воспроизводит SD->SD Copy из фактического packet capture. Пока
   * FINDNEXT исходного каталога владеет единственным FILEX-мостом, другой
   * сеанс Проводника создаёт каталог назначения. CREATE должен дождаться
   * старшего QUERY_DIRECTORY в общей FIFO, а не получить ACCESS_DENIED. Сервер
   * этого отдельного теста запускается с directory-delay=250 мс. */
  printf("\n--- TEST 32: MKDIR waits behind active FINDNEXT ---\n");
  {
    enum { seed_count = 12 };
    const char *directory = test_directory[0] == '\0' ? "" : test_directory;
    char created_path[512];
    struct windows_compound_state opened;
    struct raw_directory_state current;
    struct raw_directory_state next;
    struct raw_create_state created;
    struct smb2_context *peer = NULL;
    struct smb2fh *seed = NULL;
    struct smb2fh *created_handle = NULL;
    struct smb2fh *directory_handle = NULL;
    int found = 0;
    int queued = 0;
    int next_completed = 0;
    DWORD create_elapsed_ms = 0;
    int test_ok = 1;
    int index;

    memset(&opened, 0, sizeof(opened));
    memset(&current, 0, sizeof(current));
    memset(&next, 0, sizeof(next));
    memset(&created, 0, sizeof(created));
    snprintf(created_path, sizeof(created_path), "%s",
             test_path("zz_overlap_mkdir_target"));
    (void)smb2_rmdir(smb2, created_path);

    for (index = 0; test_ok && index < seed_count; ++index) {
      char name[64];
      snprintf(name, sizeof(name), "zz_mkdir_seed_%02d.tmp", index);
      seed = smb2_open(smb2, test_path(name), O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = seed != NULL;
      if (seed != NULL) {
        test_ok = smb2_close(smb2, seed) == 0 && test_ok;
        seed = NULL;
      }
    }

    if (test_ok) {
      peer = connect_context(server, share);
      test_ok = peer != NULL;
    }
    if (test_ok) {
      test_ok = run_windows_directory_compound(
                    smb2, directory, &opened, NULL, NULL) == 0 &&
                (uint32_t)opened.status[0] == SMB2_STATUS_SUCCESS;
    }

    /* Находим одну из тестовых записей, затем запускаем следующий физический
     * FINDNEXT и оставляем его активным на 25 мс перед точным CREATE. */
    while (test_ok && !found) {
      if (raw_query_directory_entries(smb2, opened.file_id, 0, &current) != 0 ||
          (uint32_t)current.status != SMB2_STATUS_SUCCESS ||
          !current.payload_valid) {
        test_ok = 0;
        break;
      }
      found = strncmp(current.first_name, "zz_mkdir_seed_", 14) == 0;
    }
    test_ok = test_ok && found;
    if (test_ok) {
      queued = queue_raw_query_directory_entries(
                   smb2, opened.file_id, 0, &next) == 0;
      test_ok = queued;
    }
    if (test_ok) {
      test_ok = service_for(smb2, 25) == 0;
    }
    if (test_ok) {
      const DWORD create_started = GetTickCount();
      test_ok = raw_windows_create_directory(peer, created_path, &created) == 0;
      create_elapsed_ms = GetTickCount() - create_started;
      test_ok = test_ok &&
                (uint32_t)created.status == SMB2_STATUS_SUCCESS &&
                create_elapsed_ms >= 150 && create_elapsed_ms < 5000;
    }
    if (queued) {
      next_completed = wait_for_count(smb2, &next.done, 1, 10000) == 0;
      test_ok = next_completed && test_ok;
    }

    printf("  CREATE status=%08x elapsed=%lu ms next-QD=%s status=%08x\n",
           (unsigned)created.status, (unsigned long)create_elapsed_ms,
           next_completed ? "completed" : "failed", (unsigned)next.status);

    if ((uint32_t)created.status == SMB2_STATUS_SUCCESS) {
      created_handle = smb2_fh_from_file_id(peer, &created.file_id);
      if (created_handle != NULL) {
        test_ok = smb2_close(peer, created_handle) == 0 && test_ok;
        created_handle = NULL;
      } else {
        test_ok = 0;
      }
    }
    if ((uint32_t)opened.status[0] == SMB2_STATUS_SUCCESS) {
      directory_handle = smb2_fh_from_file_id(smb2, &opened.file_id);
      if (directory_handle != NULL) {
        test_ok = smb2_close(smb2, directory_handle) == 0 && test_ok;
        directory_handle = NULL;
      }
    }
    if ((uint32_t)created.status == SMB2_STATUS_SUCCESS) {
      test_ok = smb2_rmdir(peer, created_path) == 0 && test_ok;
    }
    if (peer != NULL) {
      smb2_disconnect_share(peer);
      smb2_destroy_context(peer);
    }
    for (index = 0; index < seed_count; ++index) {
      char name[64];
      snprintf(name, sizeof(name), "zz_mkdir_seed_%02d.tmp", index);
      (void)smb2_unlink(smb2, test_path(name));
    }
    printf("RESULT TEST 32: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_33:
  /* ТЕСТ 33 воспроизводит скрытую очередь из чистой SD->SD трассы. Сервер уже
   * ответил SUCCESS на буферизованный WRITE второго Open, поэтому на проводе
   * нет незавершённых SMB-команд, но физический FILEX ещё пишет. Финальный
   * CLOSE первого файла с отложенными BASIC_INFORMATION не имеет права
   * превращать внутренний bridge-busy в IO_DEVICE_ERROR. Отдельный сервер
   * этого теста запускается с медленным каналом. */
  printf("\n--- TEST 33: CLOSE waits behind early-replied physical WRITE ---\n");
  {
    enum { file_size = 32768 };
    struct smb2fh *closing = NULL;
    struct smb2fh *background = NULL;
    struct smb2fh *verify = NULL;
    uint8_t *payload = (uint8_t*)malloc(file_size);
    uint8_t *readback = (uint8_t*)calloc(file_size, 1);
    int close_result = -1;
    int read_result = -1;
    int test_ok = payload != NULL && readback != NULL;
    int index;

    for (index = 0; payload != NULL && index < file_size; ++index) {
      payload[index] = test_byte((uint64_t)index);
    }
    (void)smb2_unlink(smb2, test_path("close_fifo_target.bin"));
    (void)smb2_unlink(smb2, test_path("close_fifo_background.bin"));

    if (test_ok) {
      closing = smb2_open(smb2, test_path("close_fifo_target.bin"),
                          O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = closing != NULL;
    }
    if (test_ok) {
      test_ok = smb2_ftruncate(smb2, closing, file_size) == 0 &&
                smb2_write(smb2, closing, payload, file_size) == file_size &&
                /* Первый Open должен остаться логически открытым, но уже не
                 * владеть физическим FILEX. Иначе медленный WRITE блокирует
                 * сам CREATE фонового файла, и тест не достигает нужной
                 * последовательности WRITE(second) -> CLOSE(first). */
                smb2_fsync(smb2, closing) == 0 &&
                raw_set_basic_info(smb2, closing) == 0;
    }
    if (test_ok) {
      background = smb2_open(smb2, test_path("close_fifo_background.bin"),
                             O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = background != NULL;
    }
    if (test_ok) {
      /* Обычный WRITE получает ранний SUCCESS после копирования payload в
       * PSRAM. При throttle=6000 физическая запись гарантированно остаётся
       * активной, когда сразу следом приходит CLOSE другого Open. */
      test_ok = smb2_write(smb2, background, payload, file_size) == file_size;
    }
    if (closing != NULL) {
      close_result = smb2_close(smb2, closing);
      closing = NULL;
      test_ok = close_result == 0 && test_ok;
    }
    if (background != NULL) {
      test_ok = smb2_close(smb2, background) == 0 && test_ok;
      background = NULL;
    }
    if (test_ok) {
      verify = smb2_open(smb2, test_path("close_fifo_target.bin"), O_RDONLY);
      test_ok = verify != NULL;
    }
    if (verify != NULL) {
      read_result = smb2_read(smb2, verify, readback, file_size);
      test_ok = read_result == file_size &&
                memcmp(payload, readback, file_size) == 0 && test_ok;
      test_ok = smb2_close(smb2, verify) == 0 && test_ok;
      verify = NULL;
    }

    printf("  target CLOSE=%d readback=%d error=%s\n", close_result,
           read_result, smb2_get_error(smb2));
    (void)smb2_unlink(smb2, test_path("close_fifo_target.bin"));
    (void)smb2_unlink(smb2, test_path("close_fifo_background.bin"));
    free(readback);
    free(payload);
    printf("RESULT TEST 33: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_34:
  /* ТЕСТ 34 повторяет финал одного файла из Windows SD->SD Copy:
   * CREATE нового файла -> SET_EOF -> WRITE -> SET_BASIC_INFORMATION ->
   * CLOSE. SMB обязан с первого WRITE использовать FILEX random mode=3 и
   * сохранить данные с метаданными без отдельного последовательного commit.
   * Размер и граница WRITE взяты из пропавшего clock.wmf трассы 0.6.79. */
  printf("\n--- TEST 34: random WRITE and metadata share one FILEX context ---\n");
  {
    enum { file_size = 75009, first_write = 65536 };
    struct smb2fh *created = NULL;
    struct smb2fh *verify = NULL;
    uint8_t *payload = (uint8_t*)malloc(file_size);
    uint8_t *readback = (uint8_t*)calloc(file_size, 1);
    int close_result = -1;
    int read_result = -1;
    int test_ok = payload != NULL && readback != NULL;
    int index;

    for (index = 0; payload != NULL && index < file_size; ++index) {
      payload[index] = test_byte((uint64_t)index);
    }
    (void)smb2_unlink(smb2, test_path("sequential_metadata_close.bin"));

    if (test_ok) {
      created = smb2_open(smb2, test_path("sequential_metadata_close.bin"),
                          O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = created != NULL;
    }
    if (test_ok) {
      test_ok =
          smb2_ftruncate(smb2, created, file_size) == 0 &&
          smb2_write(smb2, created, payload, first_write) == first_write &&
          smb2_write(smb2, created, payload + first_write,
                     file_size - first_write) == file_size - first_write &&
          raw_set_basic_info(smb2, created) == 0;
    }
    if (created != NULL) {
      close_result = smb2_close(smb2, created);
      created = NULL;
      test_ok = close_result == 0 && test_ok;
    }
    if (test_ok) {
      verify = smb2_open(smb2, test_path("sequential_metadata_close.bin"),
                         O_RDONLY);
      test_ok = verify != NULL;
    }
    if (verify != NULL) {
      int total = 0;
      while (total < file_size) {
        const int part = smb2_read(smb2, verify, readback + total,
                                   file_size - total > first_write
                                       ? first_write
                                       : file_size - total);
        if (part <= 0) break;
        total += part;
      }
      read_result = total;
      test_ok = total == file_size &&
                memcmp(payload, readback, file_size) == 0 && test_ok;
      test_ok = smb2_close(smb2, verify) == 0 && test_ok;
      verify = NULL;
    }

    printf("  random CLOSE=%d readback=%d error=%s\n", close_result,
           read_result, smb2_get_error(smb2));
    (void)smb2_unlink(smb2, test_path("sequential_metadata_close.bin"));
    free(readback);
    free(payload);
    printf("RESULT TEST 34: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

  if (only_test != NULL) goto done;

test_35:
  /* ТЕСТ 35 повторяет единственный пропавший файл аппаратной SD->SD копии
   * 0.6.80: SET_EOF 44032 -> WRITE 44032 -> SET_BASIC_INFORMATION -> CLOSE.
   * Z80-симулятор намеренно отклоняет CLOSE, если сервер выбрал для этого
   * имени последовательный mode=1. Надёжный путь обязан с первого WRITE
   * открыть FILEX mode=3, физически подтвердить все окна и сохранить файл. */
  printf("\n--- TEST 35: new SMB file avoids deferred sequential CLOSE ---\n");
  {
    enum { file_size = 44032 };
    struct smb2fh *created = NULL;
    struct smb2fh *verify = NULL;
    uint8_t *payload = (uint8_t*)malloc(file_size);
    uint8_t *readback = (uint8_t*)calloc(file_size, 1);
    int close_result = -1;
    int read_result = -1;
    int test_ok = payload != NULL && readback != NULL;
    int index;

    for (index = 0; payload != NULL && index < file_size; ++index) {
      payload[index] = test_byte((uint64_t)index + 0x680ULL);
    }
    (void)smb2_unlink(smb2, test_path("sequential_close_failure.bin"));

    if (test_ok) {
      created = smb2_open(smb2, test_path("sequential_close_failure.bin"),
                          O_CREAT | O_TRUNC | O_WRONLY);
      test_ok = created != NULL;
    }
    if (test_ok) {
      test_ok =
          smb2_ftruncate(smb2, created, file_size) == 0 &&
          smb2_write(smb2, created, payload, file_size) == file_size &&
          raw_set_basic_info(smb2, created) == 0;
    }
    if (created != NULL) {
      close_result = smb2_close(smb2, created);
      created = NULL;
      test_ok = close_result == 0 && test_ok;
    }
    if (test_ok) {
      verify = smb2_open(smb2, test_path("sequential_close_failure.bin"),
                         O_RDONLY);
      test_ok = verify != NULL;
    }
    if (verify != NULL) {
      int total = 0;
      while (total < file_size) {
        const int part = smb2_read(smb2, verify, readback + total,
                                   file_size - total);
        if (part <= 0) break;
        total += part;
      }
      read_result = total;
      test_ok = total == file_size &&
                memcmp(payload, readback, file_size) == 0 && test_ok;
      test_ok = smb2_close(smb2, verify) == 0 && test_ok;
      verify = NULL;
    }

    printf("  random WRITE CLOSE=%d readback=%d error=%s\n", close_result,
           read_result, smb2_get_error(smb2));
    (void)smb2_unlink(smb2, test_path("sequential_close_failure.bin"));
    free(readback);
    free(payload);
    printf("RESULT TEST 35: %s\n", test_ok ? "PASS" : "FAIL");
    failures += !test_ok;
  }

done:
  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  return failures == 0 ? 0 : 1;
}
