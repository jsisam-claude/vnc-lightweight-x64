/*
 * qemu_audio — the QEMU Audio VNC extension (pseudo-encoding -259), which
 * libvncclient does not implement, so we add it ourselves.
 *
 * This header is deliberately free of any libvncclient dependency: the wire
 * logic is pure byte manipulation over read/write callbacks, so it is unit- and
 * fuzz-tested directly (tests/fuzz + ipc_test). client.c provides the thin glue
 * that plugs it into a libvncclient extension.
 *
 * Wire protocol (big-endian, RFB): QEMU messages use type 255.
 *   Client->server: 255, submsg=1 (AUDIO), U16 op:
 *     op 0 ENABLE, op 1 DISABLE, op 2 SET_FORMAT{ U8 fmt, U8 channels, U32 freq }
 *   Server->client: 255, submsg=1 (AUDIO), U16 op:
 *     op 0 END, op 1 BEGIN, op 2 DATA{ U32 len, len PCM bytes }
 */
#ifndef VNC_CORE_QEMU_AUDIO_H
#define VNC_CORE_QEMU_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VNC_ENCODING_QEMU_AUDIO 0xFFFFFEFDu /* -259 */

/* Client audio operations. */
#define QA_CLIENT_ENABLE     0
#define QA_CLIENT_DISABLE    1
#define QA_CLIENT_SET_FORMAT 2

/* Server audio operations. */
#define QA_SERVER_END   0
#define QA_SERVER_BEGIN 1
#define QA_SERVER_DATA  2

/* QEMU audio sample formats. */
#define QA_FORMAT_U8  0
#define QA_FORMAT_S8  1
#define QA_FORMAT_U16 2
#define QA_FORMAT_S16 3
#define QA_FORMAT_U32 4
#define QA_FORMAT_S32 5

/* Hard cap on a single DATA payload (defense against a hostile server). */
#define QA_MAX_DATA (1u << 20) /* 1 MiB */

/* Bounds enforced on a SET_FORMAT we would accept/echo. */
#define QA_MIN_FREQ 8000u
#define QA_MAX_FREQ 192000u
#define QA_MAX_CHANNELS 8u

/* Read exactly n bytes into dst; return false on short read/error. */
typedef bool (*qa_read_fn)(void *ctx, void *dst, uint32_t n);

typedef struct {
    void *user;
    void (*on_begin)(void *user);
    void (*on_end)(void *user);
    void (*on_data)(void *user, const uint8_t *pcm, uint32_t len);
} qa_sink;

/* Per-connection parser state (tracks BEGIN/END so DATA outside a playback
 * window is dropped, not delivered). */
typedef struct {
    int playing;
} qa_state;

/*
 * Parse ONE server audio message body (the 255 type byte is already consumed by
 * the caller). Reads submsg + op (+ DATA length/payload) via `read`. Enforces
 * QA_MAX_DATA and drops DATA received outside a BEGIN/END window. Returns true
 * if the message was consumed cleanly, false on any protocol violation (the
 * caller must then disconnect — fail closed).
 */
bool qemu_audio_parse_server_msg(qa_state *st, qa_read_fn read, void *rctx,
                                 const qa_sink *sink);

/* Build client->server messages into `out` (>= 10 bytes). Sets *out_len. */
void qemu_audio_build_enable(uint8_t *out, size_t *out_len);
void qemu_audio_build_disable(uint8_t *out, size_t *out_len);
void qemu_audio_build_set_format(uint8_t *out, size_t *out_len,
                                 uint8_t fmt, uint8_t channels, uint32_t freq);

#endif /* VNC_CORE_QEMU_AUDIO_H */
