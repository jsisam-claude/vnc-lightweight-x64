/*
 * qemu_audio_fuzz — libFuzzer harness over the QEMU audio server-message parser.
 *
 * Build: cmake -DENABLE_FUZZ=ON with a clang toolchain, then run
 *   ./qemu_audio_fuzz tests/fuzz/corpus/audio
 * The parser must never read out of bounds, over-allocate, or crash on any
 * input; ASan+UBSan are linked in.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/qemu_audio.h"

typedef struct { const uint8_t *p; size_t remaining; } mem_reader;

static bool mem_read(void *ctx, void *dst, uint32_t n)
{
    mem_reader *m = ctx;
    if (n > m->remaining)
        return false;
    memcpy(dst, m->p, n);
    m->p += n;
    m->remaining -= n;
    return true;
}

static void sink_data(void *u, const uint8_t *p, uint32_t n)
{
    /* Touch the bytes so ASan validates the delivered range. */
    volatile uint8_t acc = 0;
    for (uint32_t i = 0; i < n; i++) acc = (uint8_t)(acc + p[i]);
    (void)u; (void)acc;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Parse a stream of back-to-back server messages until input is exhausted,
     * mirroring how the extension loop is driven on a live connection. */
    mem_reader m = { data, size };
    qa_state st;
    memset(&st, 0, sizeof(st));
    qa_sink sink = { NULL, NULL, NULL, sink_data };
    while (m.remaining > 0) {
        if (!qemu_audio_parse_server_msg(&st, mem_read, &m, &sink))
            break;
    }
    return 0;
}
