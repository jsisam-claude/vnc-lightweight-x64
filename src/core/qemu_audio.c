#include "core/qemu_audio.h"

#include <string.h>

/* Big-endian helpers (RFB is big-endian on the wire). */
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

bool qemu_audio_parse_server_msg(qa_state *st, qa_read_fn read, void *rctx,
                                 const qa_sink *sink)
{
    uint8_t hdr[3];
    if (!read(rctx, hdr, 3)) /* submsg (1 byte) + op (U16) */
        return false;
    if (hdr[0] != 1) /* only the AUDIO submessage exists server->client */
        return false;

    uint16_t op = get_u16(hdr + 1);
    switch (op) {
    case QA_SERVER_BEGIN:
        if (!st->playing) {
            st->playing = 1;
            if (sink && sink->on_begin)
                sink->on_begin(sink->user);
        }
        return true;

    case QA_SERVER_END:
        if (st->playing) {
            st->playing = 0;
            if (sink && sink->on_end)
                sink->on_end(sink->user);
        }
        return true;

    case QA_SERVER_DATA: {
        uint8_t lenb[4];
        if (!read(rctx, lenb, 4))
            return false;
        uint32_t len = get_u32(lenb);
        if (len > QA_MAX_DATA)
            return false; /* hostile / absurd length -> disconnect */
        if (len == 0)
            return true;

        /* Read the payload in bounded chunks (delivering only inside a playback
         * window, else discarding) so the stream stays aligned regardless of
         * state and memory use is bounded no matter how large `len` is. */
        uint8_t buf[4096];
        uint32_t remaining = len;
        while (remaining > 0) {
            uint32_t chunk = remaining < sizeof(buf) ? remaining : (uint32_t)sizeof(buf);
            if (!read(rctx, buf, chunk))
                return false;
            if (st->playing && sink && sink->on_data)
                sink->on_data(sink->user, buf, chunk);
            remaining -= chunk;
        }
        return true;
    }

    default:
        return false; /* unknown op has no length -> cannot resync -> fail */
    }
}

void qemu_audio_build_enable(uint8_t *out, size_t *out_len)
{
    out[0] = 255; out[1] = 1; put_u16(out + 2, QA_CLIENT_ENABLE);
    *out_len = 4;
}

void qemu_audio_build_disable(uint8_t *out, size_t *out_len)
{
    out[0] = 255; out[1] = 1; put_u16(out + 2, QA_CLIENT_DISABLE);
    *out_len = 4;
}

void qemu_audio_build_set_format(uint8_t *out, size_t *out_len,
                                 uint8_t fmt, uint8_t channels, uint32_t freq)
{
    out[0] = 255; out[1] = 1; put_u16(out + 2, QA_CLIENT_SET_FORMAT);
    out[4] = fmt;
    out[5] = channels;
    put_u32(out + 6, freq);
    *out_len = 10;
}
