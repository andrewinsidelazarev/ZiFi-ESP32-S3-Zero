/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/*
 * Минимальное внутреннее ядро DCE/RPC с префиксом символов libsmb2_.
 * Здесь оставлены только read-only вызовы srvsvc, которые реально делает
 * Проводник Windows; полная административная реализация по-прежнему находится
 * в отдельной библиотеке libdcerpc.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
/* Minimal build: never pull full interface tables into libsmb2. */
#undef HAVE_DCERPC_FULL
/* Stop nested #include "config.h" inside dcerpc.c from restoring defines. */
#ifdef HAVE_CONFIG_H
#undef HAVE_CONFIG_H
#define LIBSMB2_DCERPC_RESTORE_CONFIG_H 1
#endif
#include "libsmb2-dcerpc-prefix.h"
#if defined(__GNUC__)
/* Старый генератор libdcerpc объявляет имена полей как char*, хотя никогда
 * их не меняет. Скрываем только это предупреждение внутри стороннего файла. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
#endif
#include "../libdcerpc/dcerpc.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#ifdef LIBSMB2_DCERPC_RESTORE_CONFIG_H
#define HAVE_CONFIG_H 1
#endif

#include <smb2/libsmb2-dcerpc-server.h>

/* These generated helper coders are intentionally not part of the public
 * minimal srvsvc header, but the server-side wrapper in this translation
 * unit needs them to preserve NDR pointer presence. The prefix header above
 * gives the declarations the same private libsmb2_ names as their objects. */
int srvsvc_SHARE_ENUM_STRUCT_coder(char *name,
                                   struct dcerpc_context *dce,
                                   struct dcerpc_pdu *pdu,
                                   struct smb2_iovec *iov, int *offset,
                                   void *ptr);
int srvsvc_SHARE_ENUM_STRUCT_struct_coder(char *name,
                                          struct dcerpc_context *dce,
                                          struct dcerpc_pdu *pdu,
                                          struct smb2_iovec *iov, int *offset,
                                          void *ptr);

/* Реализация находится в диагностическом C++-модуле прошивки. Простое
 * C-связывание не заставляет изолированную библиотеку искать заголовки
 * верхнего проекта и сохраняет libsmb2 независимой от Arduino-классов.
 * При ZIFI_DIAGNOSTIC_LOG=0 макрос удаляет и вызовы, и их строки из образа. */
#if ZIFI_DIAGNOSTIC_LOG
extern void zifi_diagnostic_log_rpc_stage(const char *stage,
                                          int32_t value_a, int32_t value_b);
#else
#define zifi_diagnostic_log_rpc_stage(...) ((void)0)
#endif

/* UUID записаны так, как они идут по проводу DCE/RPC: первые три поля
 * имеют little-endian порядок. */
static const uint8_t zifi_srvsvc_uuid[16] = {
        0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01,
        0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88
};
static const uint8_t zifi_ndr32_uuid[16] = {
        0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11,
        0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60
};
static char zifi_ipc_share[] = "IPC$";
static char zifi_disk_remark[] = "ZX Evo SD Card";
static char zifi_ipc_remark[] = "Remote IPC";
static char zifi_empty_string[] = "";

/* Connection-oriented DCE/RPC fault codes from MS-RPCE/C706. A valid RPC
 * method failure must travel inside a FAULT PDU; returning an SMB error makes
 * Windows treat the named pipe as broken and bind it again forever. */
#define ZIFI_NCA_S_FAULT_NDR               0x1c000006U
#define ZIFI_NCA_S_INVALID_PRES_CONTEXT_ID 0x1c00001cU
#define ZIFI_NCA_S_OP_RNG_ERROR            0x1c010002U
#define ZIFI_NCA_S_PROTO_ERROR             0x1c01000bU

static uint16_t
zifi_rpc_get_le16(const uint8_t *data)
{
        return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t
zifi_rpc_get_le32(const uint8_t *data)
{
        return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
               ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void
zifi_rpc_put_le16(uint8_t *data, uint16_t value)
{
        data[0] = (uint8_t)value;
        data[1] = (uint8_t)(value >> 8);
}

static void
zifi_rpc_put_le32(uint8_t *data, uint32_t value)
{
        data[0] = (uint8_t)value;
        data[1] = (uint8_t)(value >> 8);
        data[2] = (uint8_t)(value >> 16);
        data[3] = (uint8_t)(value >> 24);
}

static void
zifi_rpc_request_info_init(struct smb2_dcerpc_request_info *request_info,
                           const uint8_t *request, size_t request_length,
                           uint16_t accepted_context)
{
        if (request_info == NULL) {
                return;
        }
        memset(request_info, 0, sizeof(*request_info));
        request_info->pdu_type = request_length >= 3 && request != NULL
                                     ? request[2] : 0xff;
        request_info->response_pdu_type = 0xff;
        request_info->context_id = 0xffff;
        request_info->opnum = 0xffff;
        request_info->accepted_context = accepted_context;
}

static int
zifi_rpc_fault(uint32_t call_id, uint16_t context_id, uint32_t status,
               uint8_t *response, size_t response_capacity,
               size_t *response_length,
               struct smb2_dcerpc_request_info *request_info)
{
        const size_t fault_length = 32;

        if (response_capacity < fault_length) {
                return -ENOSPC;
        }
        memset(response, 0, fault_length);
        response[0] = 5;
        response[2] = PDU_TYPE_FAULT;
        response[3] = PFC_FIRST_FRAG | PFC_LAST_FRAG | PFC_DID_NOT_EXECUTE;
        response[4] = 0x10; /* little-endian, ASCII, IEEE */
        zifi_rpc_put_le16(response + 8, (uint16_t)fault_length);
        zifi_rpc_put_le32(response + 12, call_id);
        zifi_rpc_put_le32(response + 16, 0); /* no fault stub data */
        zifi_rpc_put_le16(response + 20, context_id);
        zifi_rpc_put_le32(response + 24, status);
        *response_length = fault_length;
        if (request_info != NULL) {
                request_info->response_pdu_type = PDU_TYPE_FAULT;
                request_info->fault_status = status;
        }
        return 0;
}

struct zifi_rpc_share_get_info_request {
        char *server_name;
        char *net_name;
        uint32_t level;
};

struct zifi_rpc_share_check_request {
        char *server_name;
        char *device;
};

struct zifi_rpc_server_get_info_request {
        char *server_name;
        uint32_t level;
};

struct zifi_rpc_share_get_info_response {
        uint32_t level;
        union srvsvc_SHARE_INFO info;
        uint32_t status;
};

struct zifi_rpc_share_check_response {
        uint32_t type;
        uint32_t status;
};

struct zifi_rpc_server_get_info_response {
        uint32_t level;
        struct srvsvc_SERVER_INFO_101 info;
        uint32_t status;
};

/* The generated NetrShareEnum structures store only the ResumeHandle value,
 * so decoding a NULL unique pointer and a pointer to zero produces the same
 * C value.  The wire distinction is significant: MS-SRVS requires a NULL
 * input ResumeHandle to remain NULL in the response. */
struct zifi_rpc_share_enum_request {
        char *server_name;
        struct srvsvc_SHARE_ENUM_STRUCT ses;
        uint32_t preferred_maximum_length;
        uint32_t resume_handle;
        int resume_handle_present;
};

struct zifi_rpc_share_enum_response {
        struct srvsvc_NetrShareEnum_rep rep;
        int resume_handle_present;
};

static char zifi_server_comment[] = "ZiFi ESP32-S3 SMB Server";

static int
zifi_rpc_ascii_equal_no_case(const char *left, const char *right)
{
        if (left == NULL || right == NULL) {
                return 0;
        }
        while (*left != 0 && *right != 0) {
                unsigned char a = (unsigned char)*left++;
                unsigned char b = (unsigned char)*right++;
                if (a >= 'a' && a <= 'z') {
                        a = (unsigned char)(a - 'a' + 'A');
                }
                if (b >= 'a' && b <= 'z') {
                        b = (unsigned char)(b - 'a' + 'A');
                }
                if (a != b) {
                        return 0;
                }
        }
        return *left == 0 && *right == 0;
}

static int
zifi_rpc_share_get_info_request_coder(char *name,
                                      struct dcerpc_context *dce,
                                      struct dcerpc_pdu *pdu,
                                      struct smb2_iovec *iov, int *offset,
                                      void *ptr)
{
        struct zifi_rpc_share_get_info_request *request = ptr;

        if (dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &request->server_name, PTR_UNIQUE,
                             dcerpc_utf16z_coder) ||
            dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &request->net_name, PTR_REF,
                             dcerpc_utf16z_coder) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &request->level)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_share_check_request_coder(char *name,
                                   struct dcerpc_context *dce,
                                   struct dcerpc_pdu *pdu,
                                   struct smb2_iovec *iov, int *offset,
                                   void *ptr)
{
        struct zifi_rpc_share_check_request *request = ptr;

        if (dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &request->server_name, PTR_UNIQUE,
                             dcerpc_utf16z_coder) ||
            dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &request->device, PTR_REF,
                             dcerpc_utf16z_coder)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_server_get_info_request_coder(char *name,
                                       struct dcerpc_context *dce,
                                       struct dcerpc_pdu *pdu,
                                       struct smb2_iovec *iov, int *offset,
                                       void *ptr)
{
        struct zifi_rpc_server_get_info_request *request = ptr;

        if (dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &request->server_name, PTR_UNIQUE,
                             dcerpc_utf16z_coder) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &request->level)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_share_enum_resume_handle_coder(char *name,
                                        struct dcerpc_context *dce,
                                        struct dcerpc_pdu *pdu,
                                        struct smb2_iovec *iov, int *offset,
                                        void *ptr)
{
        struct zifi_rpc_share_enum_request *request = ptr;

        request->resume_handle_present = 1;
        return dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                   &request->resume_handle);
}

static int
zifi_rpc_share_enum_request_coder(char *name,
                                  struct dcerpc_context *dce,
                                  struct dcerpc_pdu *pdu,
                                  struct smb2_iovec *iov, int *offset,
                                  void *ptr)
{
        struct zifi_rpc_share_enum_request *request = ptr;

        if (dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &request->server_name, PTR_UNIQUE,
                             dcerpc_utf16z_coder) ||
            dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &request->ses, PTR_REF,
                             srvsvc_SHARE_ENUM_STRUCT_struct_coder) ||
            dcerpc_ptr_coder(name, dce, pdu, iov,
                             offset, &request->preferred_maximum_length,
                             PTR_REF, dcerpc_uint32_coder) ||
            dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             request, PTR_UNIQUE,
                             zifi_rpc_share_enum_resume_handle_coder)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_share_enum_response_coder(char *name,
                                   struct dcerpc_context *dce,
                                   struct dcerpc_pdu *pdu,
                                   struct smb2_iovec *iov, int *offset,
                                   void *ptr)
{
        struct zifi_rpc_share_enum_response *reply = ptr;
        void *resume_handle = reply->resume_handle_present
                                      ? &reply->rep.resume_handle
                                      : NULL;

        if (dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &reply->rep.ses, PTR_REF,
                             srvsvc_SHARE_ENUM_STRUCT_coder) ||
            dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             &reply->rep.total_entries, PTR_REF,
                             dcerpc_uint32_coder) ||
            dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                             resume_handle, PTR_UNIQUE,
                             dcerpc_uint32_coder) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &reply->rep.status)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_share_info_0_struct_coder(char *name,
                                   struct dcerpc_context *dce,
                                   struct dcerpc_pdu *pdu,
                                   struct smb2_iovec *iov, int *offset,
                                   void *ptr)
{
        return dcerpc_struct_coder(name, dce, pdu, iov, offset, ptr,
                                   srvsvc_SHARE_INFO_0_coder);
}

static int
zifi_rpc_share_info_1_struct_coder(char *name,
                                   struct dcerpc_context *dce,
                                   struct dcerpc_pdu *pdu,
                                   struct smb2_iovec *iov, int *offset,
                                   void *ptr)
{
        return dcerpc_struct_coder(name, dce, pdu, iov, offset, ptr,
                                   srvsvc_SHARE_INFO_1_coder);
}

static int
zifi_rpc_share_info_2_struct_coder(char *name,
                                   struct dcerpc_context *dce,
                                   struct dcerpc_pdu *pdu,
                                   struct smb2_iovec *iov, int *offset,
                                   void *ptr)
{
        return dcerpc_struct_coder(name, dce, pdu, iov, offset, ptr,
                                   srvsvc_SHARE_INFO_2_coder);
}

static int
zifi_rpc_share_info_union_coder(char *name, struct dcerpc_context *dce,
                                struct dcerpc_pdu *pdu,
                                struct smb2_iovec *iov, int *offset,
                                void *ptr)
{
        union srvsvc_SHARE_INFO *info = ptr;

        switch (dcerpc_get_switch_is(pdu)) {
        case 0:
                return dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                                        &info->ShareInfo0, PTR_UNIQUE,
                                        zifi_rpc_share_info_0_struct_coder);
        case 1:
                return dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                                        &info->ShareInfo1, PTR_UNIQUE,
                                        zifi_rpc_share_info_1_struct_coder);
        case 2:
                return dcerpc_ptr_coder(name, dce, pdu, iov, offset,
                                        &info->ShareInfo2, PTR_UNIQUE,
                                        zifi_rpc_share_info_2_struct_coder);
        default:
                return -1;
        }
}

static int
zifi_rpc_share_info_struct_coder(char *name, struct dcerpc_context *dce,
                                 struct dcerpc_pdu *pdu,
                                 struct smb2_iovec *iov, int *offset,
                                 void *ptr)
{
        uint32_t level = dcerpc_get_switch_is(pdu);
        return dcerpc_union_coder(name, dce, pdu, iov, offset, &level, ptr,
                                  zifi_rpc_share_info_union_coder);
}

static int
zifi_rpc_share_get_info_response_coder(char *name,
                                       struct dcerpc_context *dce,
                                       struct dcerpc_pdu *pdu,
                                       struct smb2_iovec *iov, int *offset,
                                       void *ptr)
{
        struct zifi_rpc_share_get_info_response *reply = ptr;

        dcerpc_set_switch_is(pdu, reply->level);
        if (dcerpc_ptr_coder(name, dce, pdu, iov, offset, &reply->info,
                             PTR_REF, zifi_rpc_share_info_struct_coder) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &reply->status)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_share_check_response_coder(char *name,
                                    struct dcerpc_context *dce,
                                    struct dcerpc_pdu *pdu,
                                    struct smb2_iovec *iov, int *offset,
                                    void *ptr)
{
        struct zifi_rpc_share_check_response *reply = ptr;

        if (dcerpc_ptr_coder(name, dce, pdu, iov, offset, &reply->type,
                             PTR_REF, dcerpc_uint32_coder) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &reply->status)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_server_info_101_coder(char *name, struct dcerpc_context *dce,
                               struct dcerpc_pdu *pdu,
                               struct smb2_iovec *iov, int *offset,
                               void *ptr)
{
        struct srvsvc_SERVER_INFO_101 *info = ptr;

        if (dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &info->platform_id) ||
            dcerpc_ptr_coder(name, dce, pdu, iov, offset, &info->name,
                             PTR_UNIQUE, dcerpc_utf16z_coder) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &info->version_major) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &info->version_minor) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset, &info->type) ||
            dcerpc_ptr_coder(name, dce, pdu, iov, offset, &info->comment,
                             PTR_UNIQUE, dcerpc_utf16z_coder)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_server_info_101_struct_coder(char *name,
                                      struct dcerpc_context *dce,
                                      struct dcerpc_pdu *pdu,
                                      struct smb2_iovec *iov, int *offset,
                                      void *ptr)
{
        return dcerpc_struct_coder(name, dce, pdu, iov, offset, ptr,
                                   zifi_rpc_server_info_101_coder);
}

static int
zifi_rpc_server_info_union_coder(char *name, struct dcerpc_context *dce,
                                 struct dcerpc_pdu *pdu,
                                 struct smb2_iovec *iov, int *offset,
                                 void *ptr)
{
        if (dcerpc_get_switch_is(pdu) != 101) {
                return -1;
        }
        return dcerpc_ptr_coder(name, dce, pdu, iov, offset, ptr,
                                PTR_UNIQUE,
                                zifi_rpc_server_info_101_struct_coder);
}

static int
zifi_rpc_server_info_struct_coder(char *name, struct dcerpc_context *dce,
                                  struct dcerpc_pdu *pdu,
                                  struct smb2_iovec *iov, int *offset,
                                  void *ptr)
{
        uint32_t level = dcerpc_get_switch_is(pdu);
        return dcerpc_union_coder(name, dce, pdu, iov, offset, &level, ptr,
                                  zifi_rpc_server_info_union_coder);
}

static int
zifi_rpc_server_get_info_response_coder(char *name,
                                        struct dcerpc_context *dce,
                                        struct dcerpc_pdu *pdu,
                                        struct smb2_iovec *iov, int *offset,
                                        void *ptr)
{
        struct zifi_rpc_server_get_info_response *reply = ptr;

        dcerpc_set_switch_is(pdu, reply->level);
        if (dcerpc_ptr_coder(name, dce, pdu, iov, offset, &reply->info,
                             PTR_REF, zifi_rpc_server_info_struct_coder) ||
            dcerpc_uint32_coder(name, dce, pdu, iov, offset,
                                &reply->status)) {
                return -1;
        }
        return 0;
}

static int
zifi_rpc_decode_stub(struct smb2_context *smb2, const uint8_t *request,
                     uint16_t fragment_length, void *decoded,
                     dcerpc_coder coder, struct dcerpc_context *dce,
                     struct dcerpc_pdu **decoded_pdu)
{
        struct smb2_iovec iov;
        char request_name[] = "Request";
        int offset = 24;
        int result;

        memset(dce, 0, sizeof(*dce));
        dce->smb2 = smb2;
        dce->packed_drep[0] = 0x10;
        dce->tctx_id = 0;
        *decoded_pdu = dcerpc_allocate_pdu(dce, ENCODING_NDR,
                                           DCERPC_DECODE, 1);
        if (*decoded_pdu == NULL) {
                return -ENOMEM;
        }
        (*decoded_pdu)->hdr.packed_drep[0] = dce->packed_drep[0];
        iov.buf = discard_const(request);
        iov.len = fragment_length;
        iov.free = NULL;
        (*decoded_pdu)->top_level = 1;
        result = coder(request_name, dce, *decoded_pdu, &iov, &offset,
                       decoded);
        if (result != 0) {
                dcerpc_free_pdu(dce, *decoded_pdu);
                *decoded_pdu = NULL;
                return -EINVAL;
        }
        return 0;
}

static int
zifi_rpc_encode_response(struct smb2_context *smb2, uint32_t call_id,
                         uint16_t context_id, void *reply,
                         dcerpc_coder coder, uint8_t *response,
                         size_t response_capacity, size_t *response_length,
                         struct smb2_dcerpc_request_info *request_info)
{
        struct dcerpc_context dce;
        struct dcerpc_pdu *pdu;
        struct smb2_iovec iov;
        char response_name[] = "Response";
        int offset = 24;
        int result;

        if (response_capacity < 24) {
                return -ENOSPC;
        }
        memset(&dce, 0, sizeof(dce));
        dce.smb2 = smb2;
        dce.packed_drep[0] = 0x10;
        dce.tctx_id = 0;
        pdu = dcerpc_allocate_pdu(&dce, ENCODING_NDR, DCERPC_ENCODE,
                                  (int)response_capacity);
        if (pdu == NULL) {
                return -ENOMEM;
        }
        pdu->hdr.packed_drep[0] = dce.packed_drep[0];
        iov.buf = pdu->payload;
        iov.len = response_capacity;
        iov.free = NULL;
        pdu->top_level = 1;
        result = coder(response_name, &dce, pdu, &iov, &offset, reply);
        if (result != 0 || offset > UINT16_MAX ||
            (size_t)offset > response_capacity) {
                dcerpc_free_pdu(&dce, pdu);
                return result != 0 ? -EINVAL : -ENOSPC;
        }
        memset(iov.buf, 0, 24);
        iov.buf[0] = 5;
        iov.buf[2] = PDU_TYPE_RESPONSE;
        iov.buf[3] = PFC_FIRST_FRAG | PFC_LAST_FRAG;
        iov.buf[4] = 0x10;
        zifi_rpc_put_le16(iov.buf + 8, (uint16_t)offset);
        zifi_rpc_put_le32(iov.buf + 12, call_id);
        zifi_rpc_put_le32(iov.buf + 16, (uint32_t)(offset - 24));
        zifi_rpc_put_le16(iov.buf + 20, context_id);
        memcpy(response, iov.buf, (size_t)offset);
        *response_length = (size_t)offset;
        if (request_info != NULL) {
                request_info->response_pdu_type = PDU_TYPE_RESPONSE;
        }
        dcerpc_free_pdu(&dce, pdu);
        return 0;
}

static int
zifi_rpc_header(const uint8_t *request, size_t request_length,
                uint16_t *fragment_length, uint32_t *call_id)
{
        uint16_t fragment;

        if (request == NULL || request_length < 16 || request[0] != 5 ||
            request[1] != 0 || request[4] != 0x10 || request[5] != 0 ||
            request[6] != 0 || request[7] != 0) {
                return -EINVAL;
        }
        fragment = zifi_rpc_get_le16(request + 8);
        if (fragment < 16 || fragment > request_length ||
            zifi_rpc_get_le16(request + 10) != 0) {
                return -EINVAL;
        }
        *fragment_length = fragment;
        *call_id = zifi_rpc_get_le32(request + 12);
        return 0;
}

static int
zifi_rpc_bind_ack(const uint8_t *request, uint16_t fragment_length,
                  uint32_t call_id, uint16_t *presentation_context,
                  uint8_t *response, size_t response_capacity,
                  size_t *response_length)
{
        static const char secondary_address[] = "\\PIPE\\srvsvc";
        struct zifi_bind_result {
                uint16_t result;
                uint16_t reason;
        } results[4];
        size_t input_offset = 28;
        size_t output_offset;
        uint8_t context_count;
        uint8_t index;
        int accepted = 0;

        if (fragment_length < 28) {
            return -EINVAL;
        }
        context_count = request[24];
        zifi_diagnostic_log_rpc_stage("bind-enter", fragment_length,
                                      context_count);
        if (context_count == 0 || context_count > 4) {
                return -EINVAL;
        }
        memset(results, 0, sizeof(results));
        *presentation_context = 0xffff;

        for (index = 0; index < context_count; ++index) {
                uint16_t context_id;
                uint8_t transfer_count;
                int abstract_ok;
                int ndr32_ok = 0;
                uint8_t transfer;

                if (input_offset + 24 > fragment_length) {
                        return -EINVAL;
                }
                context_id = zifi_rpc_get_le16(request + input_offset);
                transfer_count = request[input_offset + 2];
                abstract_ok = memcmp(request + input_offset + 4,
                                     zifi_srvsvc_uuid, 16) == 0 &&
                              zifi_rpc_get_le16(request + input_offset + 20) == 3 &&
                              zifi_rpc_get_le16(request + input_offset + 22) == 0;
                input_offset += 24;
                if (transfer_count == 0 ||
                    input_offset + (size_t)transfer_count * 20 > fragment_length) {
                        return -EINVAL;
                }
                for (transfer = 0; transfer < transfer_count; ++transfer) {
                        const uint8_t *syntax = request + input_offset +
                                                (size_t)transfer * 20;
                        if (memcmp(syntax, zifi_ndr32_uuid, 16) == 0 &&
                            zifi_rpc_get_le32(syntax + 16) == 2) {
                                ndr32_ok = 1;
                        }
                }
                input_offset += (size_t)transfer_count * 20;

                if (!abstract_ok) {
                        results[index].result = 2;
                        results[index].reason = 1;
                } else if (!ndr32_ok || accepted) {
                        results[index].result = 2;
                        results[index].reason = 2;
                } else {
                        results[index].result = 0;
                        results[index].reason = 0;
                        *presentation_context = context_id;
                        accepted = 1;
                }
        }

        /* После 16-байтного общего заголовка идут параметры bind_ack:
         * 8 байт размеров/assoc_group, 2 байта длины строки и сама строка.
         * Поэтому строка начинается со смещения 26, а не 28. Это важно:
         * поле frag_length должно в точности совпасть с числом отправленных
         * байтов, иначе Windows ждёт ещё четыре несуществующих байта. */
        output_offset = 26 + sizeof(secondary_address);
        output_offset = (output_offset + 3) & ~(size_t)3;
        output_offset += 4 + (size_t)context_count * 24;
        if (output_offset > response_capacity || output_offset > UINT16_MAX) {
                return -ENOSPC;
        }
        memset(response, 0, output_offset);
        response[0] = 5;
        response[2] = 12; /* bind_ack */
        response[3] = 3;  /* first + last fragment */
        response[4] = 0x10;
        zifi_rpc_put_le16(response + 8, (uint16_t)output_offset);
        zifi_rpc_put_le32(response + 12, call_id);
        zifi_rpc_put_le16(response + 16, zifi_rpc_get_le16(request + 16));
        zifi_rpc_put_le16(response + 18, zifi_rpc_get_le16(request + 18));
        zifi_rpc_put_le32(response + 20, 1);
        zifi_rpc_put_le16(response + 24, (uint16_t)sizeof(secondary_address));
        memcpy(response + 26, secondary_address, sizeof(secondary_address));
        output_offset = (26 + sizeof(secondary_address) + 3) & ~(size_t)3;
        response[output_offset] = context_count;
        output_offset += 4;
        for (index = 0; index < context_count; ++index) {
                zifi_rpc_put_le16(response + output_offset, results[index].result);
                zifi_rpc_put_le16(response + output_offset + 2,
                                  results[index].reason);
                if (results[index].result == 0) {
                        memcpy(response + output_offset + 4,
                               zifi_ndr32_uuid, sizeof(zifi_ndr32_uuid));
                        zifi_rpc_put_le32(response + output_offset + 20, 2);
                }
                output_offset += 24;
        }
        *response_length = output_offset;
        zifi_diagnostic_log_rpc_stage("bind-exit", accepted,
                                      (int32_t)output_offset);
        return 0;
}

static int
zifi_rpc_decode_share_enum_request(struct smb2_context *smb2,
                                   const uint8_t *request,
                                   uint16_t fragment_length, uint32_t *level,
                                   int *resume_handle_present)
{
        struct dcerpc_context dce;
        struct dcerpc_pdu *pdu = NULL;
        struct zifi_rpc_share_enum_request share_request;
        int result;

        memset(&share_request, 0, sizeof(share_request));
        result = zifi_rpc_decode_stub(smb2, request, fragment_length,
                                      &share_request,
                                      zifi_rpc_share_enum_request_coder,
                                      &dce, &pdu);
        if (result == 0) {
                *level = share_request.ses.Level;
                *resume_handle_present =
                        share_request.resume_handle_present;
        }
        if (pdu != NULL) {
                dcerpc_free_pdu(&dce, pdu);
        }
        return result;
}

static int
zifi_rpc_share_enum_response(struct smb2_context *smb2,
                             const uint8_t *request, uint16_t fragment_length,
                             uint32_t call_id, const char *disk_share,
                             uint16_t context_id,
                             uint8_t *response, size_t response_capacity,
                             size_t *response_length,
                             struct smb2_dcerpc_request_info *request_info)
{
        struct dcerpc_context dce;
        struct dcerpc_pdu *pdu;
        struct smb2_iovec iov;
        struct zifi_rpc_share_enum_response reply;
        struct srvsvc_SHARE_INFO_0 level0[2];
        struct srvsvc_SHARE_INFO_1 level1[2];
        struct srvsvc_SHARE_INFO_2 level2[2];
        uint32_t level;
        int resume_handle_present;
        char response_name[] = "Response";
        int offset = 24;
        int result;

        result = zifi_rpc_decode_share_enum_request(
                smb2, request, fragment_length, &level,
                &resume_handle_present);
        zifi_diagnostic_log_rpc_stage("enum-decoded", result,
                                      result == 0 ? (int32_t)level : -1);
        if (result == -ENOMEM) {
                return result;
        }
        if (result != 0 || (level != 0 && level != 1 && level != 2)) {
                return zifi_rpc_fault(call_id, context_id,
                                      ZIFI_NCA_S_FAULT_NDR,
                                      response, response_capacity,
                                      response_length, request_info);
        }

        memset(&dce, 0, sizeof(dce));
        dce.smb2 = smb2;
        dce.packed_drep[0] = 0x10;
        dce.tctx_id = 0;
        pdu = dcerpc_allocate_pdu(&dce, ENCODING_NDR, DCERPC_ENCODE,
                                  (int)response_capacity);
        if (pdu == NULL) {
                return -ENOMEM;
        }
        /* Ответ обязан использовать тот же NDR32 little-endian порядок.
         * Кодировщики чисел читают этот признак из PDU, а не из контекста. */
        pdu->hdr.packed_drep[0] = dce.packed_drep[0];
        memset(&reply, 0, sizeof(reply));
        memset(level0, 0, sizeof(level0));
        memset(level1, 0, sizeof(level1));
        memset(level2, 0, sizeof(level2));
        reply.rep.ses.Level = level;
        reply.rep.total_entries = 2;
        reply.rep.status = 0;
        reply.resume_handle_present = resume_handle_present;

        if (level == 0) {
                level0[0].netname = discard_const(disk_share);
                level0[1].netname = zifi_ipc_share;
                reply.rep.ses.ShareEnum.Level0.EntriesRead = 2;
                reply.rep.ses.ShareEnum.Level0.share_info_0 = level0;
        } else if (level == 1) {
                level1[0].netname = discard_const(disk_share);
                level1[0].type = SRVSVC_SHARE_TYPE_DISKTREE;
                level1[0].remark = zifi_disk_remark;
                level1[1].netname = zifi_ipc_share;
                level1[1].type = SRVSVC_SHARE_TYPE_IPC |
                                 SRVSVC_SHARE_TYPE_HIDDEN;
                level1[1].remark = zifi_ipc_remark;
                reply.rep.ses.ShareEnum.Level1.EntriesRead = 2;
                reply.rep.ses.ShareEnum.Level1.share_info_1 = level1;
        } else {
                level2[0].netname = discard_const(disk_share);
                level2[0].type = SRVSVC_SHARE_TYPE_DISKTREE;
                level2[0].remark = zifi_disk_remark;
                level2[0].max_users = 0xffffffff;
                level2[0].path = discard_const(disk_share);
                level2[0].passwd = zifi_empty_string;
                level2[1].netname = zifi_ipc_share;
                level2[1].type = SRVSVC_SHARE_TYPE_IPC |
                                 SRVSVC_SHARE_TYPE_HIDDEN;
                level2[1].remark = zifi_ipc_remark;
                level2[1].max_users = 0xffffffff;
                level2[1].path = zifi_empty_string;
                level2[1].passwd = zifi_empty_string;
                reply.rep.ses.ShareEnum.Level2.EntriesRead = 2;
                reply.rep.ses.ShareEnum.Level2.share_info_2 = level2;
        }

        iov.buf = pdu->payload;
        iov.len = response_capacity;
        iov.free = NULL;
        pdu->top_level = 1;
        zifi_diagnostic_log_rpc_stage("enum-encode-enter", (int32_t)level,
                                      (int32_t)response_capacity);
        result = zifi_rpc_share_enum_response_coder(
                response_name, &dce, pdu, &iov, &offset, &reply);
        zifi_diagnostic_log_rpc_stage("enum-encode-exit", result, offset);
        if (result != 0 || offset > UINT16_MAX ||
            (size_t)offset > response_capacity) {
                dcerpc_free_pdu(&dce, pdu);
                return result != 0 ? -EINVAL : -ENOSPC;
        }

        /* Общий DCE/RPC RESPONSE заголовок. NDR-кодировщик выше
         * заполнил stub, а здесь мы привязываем его к call_id клиента. */
        memset(iov.buf, 0, 24);
        iov.buf[0] = 5;
        iov.buf[2] = 2; /* response */
        iov.buf[3] = 3; /* first + last fragment */
        iov.buf[4] = 0x10;
        zifi_rpc_put_le16(iov.buf + 8, (uint16_t)offset);
        zifi_rpc_put_le32(iov.buf + 12, call_id);
        zifi_rpc_put_le32(iov.buf + 16, (uint32_t)(offset - 24));
        zifi_rpc_put_le16(iov.buf + 20, context_id);
        memcpy(response, iov.buf, (size_t)offset);
        *response_length = (size_t)offset;
        if (request_info != NULL) {
                request_info->response_pdu_type = PDU_TYPE_RESPONSE;
        }
        dcerpc_free_pdu(&dce, pdu);
        return 0;
}

static int
zifi_rpc_share_get_info_response(struct smb2_context *smb2,
                                 const uint8_t *request,
                                 uint16_t fragment_length, uint32_t call_id,
                                 const char *disk_share, uint16_t context_id,
                                 uint8_t *response, size_t response_capacity,
                                 size_t *response_length,
                                 struct smb2_dcerpc_request_info *request_info)
{
        struct dcerpc_context decode_context;
        struct dcerpc_pdu *decode_pdu = NULL;
        struct zifi_rpc_share_get_info_request decoded;
        struct zifi_rpc_share_get_info_response reply;
        int result;
        int disk;
        int ipc;

        memset(&decoded, 0, sizeof(decoded));
        result = zifi_rpc_decode_stub(smb2, request, fragment_length, &decoded,
                                      zifi_rpc_share_get_info_request_coder,
                                      &decode_context, &decode_pdu);
        zifi_diagnostic_log_rpc_stage("share-info-decode", result,
                                      result == 0
                                              ? (int32_t)decoded.level : -1);
        if (result != 0) {
                return result == -ENOMEM
                               ? result
                               : zifi_rpc_fault(call_id, context_id,
                                                ZIFI_NCA_S_FAULT_NDR,
                                                response, response_capacity,
                                                response_length,
                                                request_info);
        }
        disk = zifi_rpc_ascii_equal_no_case(decoded.net_name, disk_share);
        ipc = zifi_rpc_ascii_equal_no_case(decoded.net_name, zifi_ipc_share);
        if (decoded.level != 0 && decoded.level != 1 && decoded.level != 2) {
                dcerpc_free_pdu(&decode_context, decode_pdu);
                return zifi_rpc_fault(call_id, context_id,
                                      ZIFI_NCA_S_FAULT_NDR, response,
                                      response_capacity, response_length,
                                      request_info);
        }

        memset(&reply, 0, sizeof(reply));
        reply.level = decoded.level;
        reply.status = disk || ipc ? 0 : 2310; /* NERR_NetNameNotFound */
        if (decoded.level == 0) {
                reply.info.ShareInfo0.netname =
                        disk ? discard_const(disk_share)
                             : (ipc ? zifi_ipc_share : NULL);
        } else if (decoded.level == 1) {
                reply.info.ShareInfo1.netname =
                        disk ? discard_const(disk_share)
                             : (ipc ? zifi_ipc_share : NULL);
                reply.info.ShareInfo1.type =
                        ipc ? SRVSVC_SHARE_TYPE_IPC |
                                      SRVSVC_SHARE_TYPE_HIDDEN
                            : SRVSVC_SHARE_TYPE_DISKTREE;
                reply.info.ShareInfo1.remark =
                        disk ? zifi_disk_remark
                             : (ipc ? zifi_ipc_remark : NULL);
        } else {
                reply.info.ShareInfo2.netname =
                        disk ? discard_const(disk_share)
                             : (ipc ? zifi_ipc_share : NULL);
                reply.info.ShareInfo2.type =
                        ipc ? SRVSVC_SHARE_TYPE_IPC |
                                      SRVSVC_SHARE_TYPE_HIDDEN
                            : SRVSVC_SHARE_TYPE_DISKTREE;
                reply.info.ShareInfo2.remark =
                        disk ? zifi_disk_remark
                             : (ipc ? zifi_ipc_remark : NULL);
                reply.info.ShareInfo2.max_users = 0xffffffff;
                reply.info.ShareInfo2.path =
                        disk ? discard_const(disk_share) : zifi_empty_string;
                reply.info.ShareInfo2.passwd = zifi_empty_string;
        }
        dcerpc_free_pdu(&decode_context, decode_pdu);
        return zifi_rpc_encode_response(
                smb2, call_id, context_id, &reply,
                zifi_rpc_share_get_info_response_coder, response,
                response_capacity, response_length, request_info);
}

static int
zifi_rpc_share_check_response(struct smb2_context *smb2,
                              const uint8_t *request,
                              uint16_t fragment_length, uint32_t call_id,
                              const char *disk_share, uint16_t context_id,
                              uint8_t *response, size_t response_capacity,
                              size_t *response_length,
                              struct smb2_dcerpc_request_info *request_info)
{
        struct dcerpc_context decode_context;
        struct dcerpc_pdu *decode_pdu = NULL;
        struct zifi_rpc_share_check_request decoded;
        struct zifi_rpc_share_check_response reply;
        int result;

        memset(&decoded, 0, sizeof(decoded));
        result = zifi_rpc_decode_stub(smb2, request, fragment_length, &decoded,
                                      zifi_rpc_share_check_request_coder,
                                      &decode_context, &decode_pdu);
        if (result != 0) {
                return result == -ENOMEM
                               ? result
                               : zifi_rpc_fault(call_id, context_id,
                                                ZIFI_NCA_S_FAULT_NDR,
                                                response, response_capacity,
                                                response_length,
                                                request_info);
        }
        memset(&reply, 0, sizeof(reply));
        if (zifi_rpc_ascii_equal_no_case(decoded.device, disk_share)) {
                reply.type = SRVSVC_SHARE_TYPE_DISKTREE;
        } else if (zifi_rpc_ascii_equal_no_case(decoded.device,
                                                zifi_ipc_share)) {
                reply.type = SRVSVC_SHARE_TYPE_IPC |
                             SRVSVC_SHARE_TYPE_HIDDEN;
        } else {
                reply.status = 2311; /* NERR_DeviceNotShared */
        }
        dcerpc_free_pdu(&decode_context, decode_pdu);
        return zifi_rpc_encode_response(
                smb2, call_id, context_id, &reply,
                zifi_rpc_share_check_response_coder, response,
                response_capacity, response_length, request_info);
}

static int
zifi_rpc_server_get_info_response(struct smb2_context *smb2,
                                  const uint8_t *request,
                                  uint16_t fragment_length, uint32_t call_id,
                                  const char *server_name,
                                  uint16_t context_id, uint8_t *response,
                                  size_t response_capacity,
                                  size_t *response_length,
                                  struct smb2_dcerpc_request_info *request_info)
{
        struct dcerpc_context decode_context;
        struct dcerpc_pdu *decode_pdu = NULL;
        struct zifi_rpc_server_get_info_request decoded;
        struct zifi_rpc_server_get_info_response reply;
        int result;

        memset(&decoded, 0, sizeof(decoded));
        result = zifi_rpc_decode_stub(smb2, request, fragment_length, &decoded,
                                      zifi_rpc_server_get_info_request_coder,
                                      &decode_context, &decode_pdu);
        zifi_diagnostic_log_rpc_stage("server-info-decode", result,
                                      result == 0
                                              ? (int32_t)decoded.level : -1);
        if (result != 0) {
                return result == -ENOMEM
                               ? result
                               : zifi_rpc_fault(call_id, context_id,
                                                ZIFI_NCA_S_FAULT_NDR,
                                                response, response_capacity,
                                                response_length,
                                                request_info);
        }
        if (decoded.level != 101) {
                dcerpc_free_pdu(&decode_context, decode_pdu);
                return zifi_rpc_fault(call_id, context_id,
                                      ZIFI_NCA_S_FAULT_NDR, response,
                                      response_capacity, response_length,
                                      request_info);
        }
        memset(&reply, 0, sizeof(reply));
        reply.level = decoded.level;
        reply.info.platform_id = SRVSVC_PLATFORM_ID_NT;
        reply.info.name = discard_const(server_name);
        reply.info.version_major = 10;
        reply.info.version_minor = 0;
        reply.info.type = SRVSVC_SV_TYPE_SERVER |
                          SRVSVC_SV_TYPE_SERVER_NT;
        reply.info.comment = zifi_server_comment;
        dcerpc_free_pdu(&decode_context, decode_pdu);
        return zifi_rpc_encode_response(
                smb2, call_id, context_id, &reply,
                zifi_rpc_server_get_info_response_coder, response,
                response_capacity, response_length, request_info);
}

int
smb2_dcerpc_srvsvc_reply(struct smb2_context *smb2,
                         const uint8_t *request, size_t request_length,
                         const char *disk_share, const char *server_name,
                         uint16_t *presentation_context,
                         uint8_t *response, size_t response_capacity,
                         size_t *response_length,
                         struct smb2_dcerpc_request_info *request_info)
{
        uint16_t fragment_length;
        uint16_t context_id;
        uint16_t opnum;
        uint32_t call_id;
        int result;

        zifi_rpc_request_info_init(request_info, request, request_length,
                                   presentation_context != NULL
                                           ? *presentation_context : 0xffff);
        if (smb2 == NULL || disk_share == NULL || *disk_share == 0 ||
            server_name == NULL || *server_name == 0 ||
            presentation_context == NULL || response == NULL ||
            response_length == NULL) {
                return -EINVAL;
        }
        *response_length = 0;
        result = zifi_rpc_header(request, request_length, &fragment_length,
                                 &call_id);
        if (result != 0) {
                return result;
        }
        if (request_info != NULL) {
                request_info->call_id = call_id;
        }
        if (request[2] == 11) {
                result = zifi_rpc_bind_ack(request, fragment_length, call_id,
                                           presentation_context, response,
                                           response_capacity, response_length);
                if (request_info != NULL) {
                        request_info->accepted_context = *presentation_context;
                        if (result == 0) {
                                request_info->response_pdu_type =
                                        PDU_TYPE_BIND_ACK;
                        }
                }
                return result;
        }
        if (request[2] == 0) {
                if (fragment_length < 24) {
                        return zifi_rpc_fault(
                                call_id, *presentation_context,
                                ZIFI_NCA_S_PROTO_ERROR, response,
                                response_capacity, response_length,
                                request_info);
                }
                context_id = zifi_rpc_get_le16(request + 20);
                opnum = zifi_rpc_get_le16(request + 22);
                if (request_info != NULL) {
                        request_info->context_id = context_id;
                        request_info->opnum = opnum;
                }
                if (context_id != *presentation_context) {
                        return zifi_rpc_fault(
                                call_id, context_id,
                                ZIFI_NCA_S_INVALID_PRES_CONTEXT_ID,
                                response, response_capacity,
                                response_length, request_info);
                }
                if ((request[3] & PFC_OBJECT_UUID) != 0 ||
                    (request[3] & (PFC_FIRST_FRAG | PFC_LAST_FRAG)) !=
                            (PFC_FIRST_FRAG | PFC_LAST_FRAG)) {
                        return zifi_rpc_fault(
                                call_id, context_id, ZIFI_NCA_S_PROTO_ERROR,
                                response, response_capacity,
                                response_length, request_info);
                }
                switch (opnum) {
                case SRVSVC_NETRSHAREENUM:
                        return zifi_rpc_share_enum_response(
                                smb2, request, fragment_length, call_id,
                                disk_share, context_id, response,
                                response_capacity, response_length,
                                request_info);
                case SRVSVC_NETRSHAREGETINFO:
                        return zifi_rpc_share_get_info_response(
                                smb2, request, fragment_length, call_id,
                                disk_share, context_id, response,
                                response_capacity, response_length,
                                request_info);
                case SRVSVC_NETRSHARECHECK:
                        return zifi_rpc_share_check_response(
                                smb2, request, fragment_length, call_id,
                                disk_share, context_id, response,
                                response_capacity, response_length,
                                request_info);
                case SRVSVC_NETRSERVERGETINFO:
                        return zifi_rpc_server_get_info_response(
                                smb2, request, fragment_length, call_id,
                                server_name, context_id, response,
                                response_capacity, response_length,
                                request_info);
                default:
                        return zifi_rpc_fault(
                                call_id, context_id,
                                ZIFI_NCA_S_OP_RNG_ERROR, response,
                                response_capacity, response_length,
                                request_info);
                }
        }
        return zifi_rpc_fault(call_id, *presentation_context,
                              ZIFI_NCA_S_PROTO_ERROR, response,
                              response_capacity, response_length,
                              request_info);
}
