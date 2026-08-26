/* SMB2 CANCEL не имеет ответа и повторяет идентификатор отменяемой команды. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include "compat.h"
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-private.h"

struct smb2_pdu *
smb2_cmd_cancel_async(struct smb2_context *smb2, uint64_t message_id,
                      uint64_t async_id)
{
        struct smb2_pdu *pdu;
        struct smb2_iovec *iov;
        uint8_t *buf;

        pdu = smb2_allocate_pdu(smb2, SMB2_CANCEL, NULL, NULL);
        if (pdu == NULL) {
                return NULL;
        }
        buf = calloc(SMB2_CANCEL_REQUEST_SIZE, sizeof(uint8_t));
        if (buf == NULL) {
                smb2_free_pdu(smb2, pdu);
                smb2_set_error(smb2, "Failed to allocate cancel buffer");
                return NULL;
        }
        iov = smb2_add_iovector(smb2, &pdu->out, buf,
                                SMB2_CANCEL_REQUEST_SIZE, free);
        if (iov == NULL) {
                free(buf);
                smb2_free_pdu(smb2, pdu);
                return NULL;
        }
        smb2_set_uint16(iov, 0, SMB2_CANCEL_REQUEST_SIZE);
        smb2_set_uint16(iov, 2, 0);
        pdu->header.message_id = message_id;
        if (async_id != 0) {
                pdu->header.flags |= SMB2_FLAGS_ASYNC_COMMAND;
                pdu->header.async.async_id = async_id;
        }
        if (smb2_pad_to_64bit(smb2, &pdu->out) != 0) {
                smb2_free_pdu(smb2, pdu);
                return NULL;
        }
        return pdu;
}
