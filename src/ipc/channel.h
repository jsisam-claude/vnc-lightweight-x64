/*
 * Duplex message channel (ipc/channel).
 *
 * Frames the protocol.h messages over a byte transport: a pair of OS handles
 * (anonymous pipes on Windows, socketpair/pipe fds on POSIX). All validation of
 * message type and length lives here, so both processes enforce identical
 * bounds. Blocking send/recv; threading is the caller's concern.
 */
#ifndef VNC_IPC_CHANNEL_H
#define VNC_IPC_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc/protocol.h"

/* OS handle: a SOCKET/HANDLE on Windows, an fd on POSIX. -1 is invalid. */
typedef intptr_t vnc_handle;

typedef struct {
    vnc_handle rd; /* read end  */
    vnc_handle wr; /* write end */
} vnc_channel;

/* Send one message. Returns false on transport error or if len exceeds the
 * per-type cap (a programming error on the send side). */
bool vnc_channel_send(vnc_channel *ch, uint32_t type,
                      const void *payload, uint32_t len);

/* Convenience: send a message with a fixed struct body plus an optional
 * variable-length tail (e.g. header + string / pixels). Either part may be
 * NULL/0. */
bool vnc_channel_send2(vnc_channel *ch, uint32_t type,
                       const void *head, uint32_t head_len,
                       const void *tail, uint32_t tail_len);

/*
 * Receive one message into caller storage. On success out_type and out_len are
 * set and up to buf_cap payload bytes are written to buf. Returns:
 *   1  message received
 *   0  peer closed cleanly (EOF)
 *  -1  transport error, or a malformed/oversized/over-cap frame (fail-closed;
 *      the caller must treat this as a hostile peer and tear down).
 */
int vnc_channel_recv(vnc_channel *ch, uint32_t *out_type,
                     void *buf, uint32_t buf_cap, uint32_t *out_len);

#endif /* VNC_IPC_CHANNEL_H */
