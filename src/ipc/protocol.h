/*
 * IPC protocol between the trusted UI process (vncviewer) and the untrusted
 * decoder worker (vncworker).
 *
 * SECURITY MODEL: the worker parses all attacker-controlled RFB data inside a
 * sandbox and is ASSUMED compromisable. This channel is therefore a trust
 * boundary in BOTH directions — every message is re-validated by its receiver
 * against the fixed bounds below before any field is interpreted. No pointers
 * ever cross the channel; only fixed-layout PODs and length-prefixed byte
 * blobs. The bulk framebuffer travels through a separate read-only-to-UI shared
 * mapping (see ipc/shm.h), never through these messages.
 *
 * Wire format: every message is a fixed 8-byte header
 *     { uint32_t type; uint32_t length; }   (little-endian, our targets are LE)
 * followed by exactly `length` payload bytes. `length` is bounded per type by
 * the receiver; anything out of range terminates the connection (fail-closed).
 */
#ifndef VNC_IPC_PROTOCOL_H
#define VNC_IPC_PROTOCOL_H

#include <stdint.h>

#define VNC_IPC_MAGIC   0x564E4331u /* "VNC1" — sanity check at handshake */
#define VNC_IPC_VERSION 1u

/* Absolute ceiling on any single message payload. Individual types are capped
 * tighter (see vnc_ipc_max_payload). */
#define VNC_IPC_MAX_PAYLOAD (1u << 20) /* 1 MiB (matches clipboard/audio caps) */

/* ---- UI -> Worker (commands) ---------------------------------------------- */
enum {
    VNC_CMD_CONFIG        = 0x0001, /* vnc_ipc_config */
    VNC_CMD_KEY           = 0x0002, /* vnc_ipc_key */
    VNC_CMD_KEY_EXT       = 0x0003, /* vnc_ipc_key_ext  (M4, QEMU ext key) */
    VNC_CMD_POINTER       = 0x0004, /* vnc_ipc_pointer */
    VNC_CMD_CUT_TEXT      = 0x0005, /* u32 len + UTF-8 bytes (to server) */
    VNC_CMD_REQUEST_UPDATE= 0x0006, /* vnc_ipc_update_req */
    VNC_CMD_PASSWORD      = 0x0007, /* u32 len + bytes (reply to PW request) */
    VNC_CMD_AUDIO_ENABLE  = 0x0008, /* vnc_ipc_audio_cfg (M5) */
    VNC_CMD_AUDIO_DISABLE = 0x0009, /* empty (M5) */
    VNC_CMD_FT_OP         = 0x000A, /* file-transfer op (M8) */
    VNC_CMD_REQUEST_RESIZE= 0x000B, /* vnc_ipc_request_resize (client-driven resize) */
    VNC_CMD_SHUTDOWN      = 0x00FF  /* empty */
};

/* ---- Worker -> UI (events) ------------------------------------------------ */
enum {
    VNC_EVT_HELLO         = 0x8000, /* vnc_ipc_hello (first message) */
    VNC_EVT_STATUS        = 0x8001, /* vnc_ipc_status */
    VNC_EVT_RESIZE        = 0x8002, /* vnc_ipc_resize (framebuffer remapped) */
    VNC_EVT_UPDATE        = 0x8003, /* vnc_ipc_rect (dirty rect in shared FB) */
    VNC_EVT_CUT_TEXT      = 0x8004, /* u32 len + UTF-8 bytes (from server) */
    VNC_EVT_CURSOR        = 0x8005, /* vnc_ipc_cursor + pixels (M3) */
    VNC_EVT_LED           = 0x8006, /* vnc_ipc_led (M4) */
    VNC_EVT_AUDIO_FORMAT  = 0x8007, /* vnc_ipc_audio_cfg (M5) */
    VNC_EVT_AUDIO_DATA    = 0x8008, /* u32 len + PCM bytes (M5) */
    VNC_EVT_AUDIO_END     = 0x8009, /* empty (M5) */
    VNC_EVT_PASSWORD_REQ  = 0x800A, /* empty — worker needs a password now */
    VNC_EVT_LOG           = 0x800B, /* u8 level + message bytes */
    VNC_EVT_FT_DATA       = 0x800C  /* file-transfer data (M8) */
};

/* Status codes for VNC_EVT_STATUS. */
enum {
    VNC_STATUS_CONNECTED    = 1,
    VNC_STATUS_DISCONNECTED = 2,
    VNC_STATUS_AUTH_FAILED  = 3,
    VNC_STATUS_CONNECT_FAILED = 4
};

#pragma pack(push, 1)

typedef struct { uint32_t type; uint32_t length; } vnc_ipc_header;

typedef struct {
    uint32_t magic;    /* VNC_IPC_MAGIC */
    uint32_t version;  /* VNC_IPC_VERSION */
} vnc_ipc_hello;

typedef struct { uint8_t code; } vnc_ipc_status;

typedef struct { uint32_t width; uint32_t height; } vnc_ipc_resize;

typedef struct { uint16_t x, y, w, h; } vnc_ipc_rect;

typedef struct {
    uint8_t  view_only;
    /* encodings string follows as the remainder of the payload (not NUL-term). */
} vnc_ipc_config;

typedef struct { uint32_t keysym; uint8_t down; } vnc_ipc_key;

typedef struct { uint32_t keysym; uint32_t keycode; uint8_t down; } vnc_ipc_key_ext;

typedef struct { uint16_t x; uint16_t y; uint8_t button_mask; } vnc_ipc_pointer;

typedef struct { uint8_t incremental; } vnc_ipc_update_req;

typedef struct { uint16_t width, height; } vnc_ipc_request_resize;

typedef struct { uint8_t led_state; } vnc_ipc_led;

typedef struct {
    int16_t  xhot, yhot;
    uint16_t width, height;
    /* width*height*4 BGRA pixels follow (already bounded by VNC_MAX_FB_*). */
} vnc_ipc_cursor;

typedef struct {
    uint8_t  sample_format; /* QEMU audio format enum */
    uint8_t  channels;      /* 1..8 */
    uint32_t frequency;     /* 8000..192000 */
} vnc_ipc_audio_cfg;

#pragma pack(pop)

/* Per-type maximum payload length, enforced by the receiver. Returns 0 for an
 * unknown type (=> reject). Implemented in channel.c so both processes agree. */
uint32_t vnc_ipc_max_payload(uint32_t type);

#endif /* VNC_IPC_PROTOCOL_H */
