#ifndef _LIBSMB2_DCERPC_SERVER_H_
#define _LIBSMB2_DCERPC_SERVER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct smb2_context;

/* Метаданные фактически принятого PDU. Они нужны серверу для точного журнала:
 * сохранённый presentation context дескриптора сам по себе не показывает,
 * какие context_id и opnum прислал клиент. Значение 0xffff означает, что поле
 * к данному типу PDU неприменимо или пакет оказался слишком коротким. */
struct smb2_dcerpc_request_info {
        uint8_t pdu_type;
        uint8_t response_pdu_type;
        uint16_t context_id;
        uint16_t opnum;
        uint16_t accepted_context;
        uint32_t call_id;
        uint32_t fault_status;
};

/* Компактная серверная часть srvsvc для ZiFi. Она принимает DCE/RPC BIND и
 * read-only вызовы, которые реально делает Проводник Windows: NetrShareEnum,
 * NetrShareGetInfo, NetrShareCheck и NetrServerGetInfo. Полную
 * административную службу встраивать в ESP не нужно. */
int smb2_dcerpc_srvsvc_reply(struct smb2_context *smb2,
                            const uint8_t *request, size_t request_length,
                            const char *disk_share, const char *server_name,
                            uint16_t *presentation_context,
                            uint8_t *response, size_t response_capacity,
                            size_t *response_length,
                            struct smb2_dcerpc_request_info *request_info);

#ifdef __cplusplus
}
#endif

#endif /* _LIBSMB2_DCERPC_SERVER_H_ */
