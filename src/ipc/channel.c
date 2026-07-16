#include "ipc/channel.h"

#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <errno.h>
#  include <unistd.h>
#endif

/* Per-type payload ceiling. Fixed-size messages are capped at exactly their
 * struct size; variable ones at their documented maximum. Unknown types => 0,
 * which makes the receiver reject them. */
uint32_t vnc_ipc_max_payload(uint32_t type)
{
    switch (type) {
    case VNC_CMD_CONFIG:         return 1u + 512u;               /* view_only + encodings */
    case VNC_CMD_KEY:            return sizeof(vnc_ipc_key);
    case VNC_CMD_KEY_EXT:        return sizeof(vnc_ipc_key_ext);
    case VNC_CMD_POINTER:        return sizeof(vnc_ipc_pointer);
    case VNC_CMD_CUT_TEXT:       return VNC_IPC_MAX_PAYLOAD;      /* 1 MiB clipboard */
    case VNC_CMD_REQUEST_UPDATE: return sizeof(vnc_ipc_update_req);
    case VNC_CMD_PASSWORD:       return 512u;
    case VNC_CMD_AUDIO_ENABLE:   return sizeof(vnc_ipc_audio_cfg);
    case VNC_CMD_AUDIO_DISABLE:  return 0u;
    case VNC_CMD_FT_OP:          return 4096u;
    case VNC_CMD_SHUTDOWN:       return 0u;

    case VNC_EVT_HELLO:          return sizeof(vnc_ipc_hello);
    case VNC_EVT_STATUS:         return sizeof(vnc_ipc_status);
    case VNC_EVT_RESIZE:         return sizeof(vnc_ipc_resize);
    case VNC_EVT_UPDATE:         return sizeof(vnc_ipc_rect);
    case VNC_EVT_CUT_TEXT:       return VNC_IPC_MAX_PAYLOAD;
    case VNC_EVT_CURSOR:         return sizeof(vnc_ipc_cursor)
                                        + (uint32_t)256 * 256 * 4; /* capped cursor */
    case VNC_EVT_LED:            return sizeof(vnc_ipc_led);
    case VNC_EVT_AUDIO_FORMAT:   return sizeof(vnc_ipc_audio_cfg);
    case VNC_EVT_AUDIO_DATA:     return VNC_IPC_MAX_PAYLOAD;
    case VNC_EVT_AUDIO_END:      return 0u;
    case VNC_EVT_PASSWORD_REQ:   return 0u;
    case VNC_EVT_LOG:            return 1u + 512u;
    case VNC_EVT_FT_DATA:        return VNC_IPC_MAX_PAYLOAD;
    default:                     return 0u; /* unknown type => reject */
    }
}

static bool write_fully(vnc_handle h, const void *buf, size_t len)
{
    const char *p = buf;
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile((HANDLE)h, p + off, (DWORD)(len - off), &n, NULL) || n == 0)
            return false;
#else
        ssize_t n = write((int)h, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
#endif
        off += (size_t)n;
    }
    return true;
}

/* Returns 1 on full read, 0 on clean EOF at a message boundary, -1 on error. */
static int read_fully(vnc_handle h, void *buf, size_t len)
{
    char *p = buf;
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile((HANDLE)h, p + off, (DWORD)(len - off), &n, NULL)) {
            DWORD e = GetLastError();
            return (e == ERROR_BROKEN_PIPE && off == 0) ? 0 : -1;
        }
        if (n == 0)
            return (off == 0) ? 0 : -1;
#else
        ssize_t n = read((int)h, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return (off == 0) ? 0 : -1; /* EOF only clean on a boundary */
#endif
        off += (size_t)n;
    }
    return 1;
}

bool vnc_channel_send(vnc_channel *ch, uint32_t type,
                      const void *payload, uint32_t len)
{
    return vnc_channel_send2(ch, type, payload, len, NULL, 0);
}

bool vnc_channel_send2(vnc_channel *ch, uint32_t type,
                       const void *head, uint32_t head_len,
                       const void *tail, uint32_t tail_len)
{
    uint32_t total = head_len + tail_len;
    if (total < head_len) /* overflow */
        return false;
    if (total > vnc_ipc_max_payload(type))
        return false;

    vnc_ipc_header hdr = { type, total };
    if (!write_fully(ch->wr, &hdr, sizeof(hdr)))
        return false;
    if (head_len && !write_fully(ch->wr, head, head_len))
        return false;
    if (tail_len && !write_fully(ch->wr, tail, tail_len))
        return false;
    return true;
}

int vnc_channel_recv(vnc_channel *ch, uint32_t *out_type,
                     void *buf, uint32_t buf_cap, uint32_t *out_len)
{
    vnc_ipc_header hdr;
    int r = read_fully(ch->rd, &hdr, sizeof(hdr));
    if (r <= 0)
        return r; /* 0 EOF, -1 error */

    uint32_t cap = vnc_ipc_max_payload(hdr.type);
    if (cap == 0 && hdr.type != VNC_CMD_AUDIO_DISABLE &&
        hdr.type != VNC_CMD_SHUTDOWN && hdr.type != VNC_EVT_AUDIO_END &&
        hdr.type != VNC_EVT_PASSWORD_REQ) {
        /* Unknown type (cap==0 and not one of the legitimately-empty types). */
        return -1;
    }
    if (hdr.length > cap || hdr.length > buf_cap)
        return -1; /* over per-type cap or won't fit caller storage */

    if (hdr.length > 0) {
        if (read_fully(ch->rd, buf, hdr.length) != 1)
            return -1;
    }
    *out_type = hdr.type;
    *out_len = hdr.length;
    return 1;
}
