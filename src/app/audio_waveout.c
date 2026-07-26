#include "app/audio_waveout.h"
#include "core/qemu_audio.h" /* QA_FORMAT_* */

#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")

#define WO_NUM_BUFFERS 16
#define WO_BUFFER_BYTES 8192
#define WO_PREBUFFER 3 /* buffers queued before playback un-pauses */

struct waveout_sink {
    HWAVEOUT hwo;
    WAVEHDR  hdr[WO_NUM_BUFFERS];
    uint8_t  buf[WO_NUM_BUFFERS][WO_BUFFER_BYTES];
    int      next;
    int      queued;   /* buffers written since last (re)start */
    BOOL     started;  /* playback un-paused */
    BOOL     ok;
};

static int qa_bits(uint8_t fmt)
{
    switch (fmt) {
    case QA_FORMAT_U8: case QA_FORMAT_S8:  return 8;
    case QA_FORMAT_U16: case QA_FORMAT_S16: return 16;
    case QA_FORMAT_U32: case QA_FORMAT_S32: return 32;
    default: return 0;
    }
}

waveout_sink *waveout_create(uint8_t qa_format, uint8_t channels, uint32_t freq)
{
    int bits = qa_bits(qa_format);
    if (bits == 0 || channels < 1 || channels > QA_MAX_CHANNELS ||
        freq < QA_MIN_FREQ || freq > QA_MAX_FREQ)
        return NULL;

    waveout_sink *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    WAVEFORMATEX wf;
    ZeroMemory(&wf, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = channels;
    wf.nSamplesPerSec = freq;
    wf.wBitsPerSample = (WORD)bits;
    wf.nBlockAlign = (WORD)(channels * bits / 8);
    wf.nAvgBytesPerSec = freq * wf.nBlockAlign;

    if (waveOutOpen(&s->hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        free(s);
        return NULL;
    }
    /* Start paused so we can prebuffer a few chunks before audio flows. */
    waveOutPause(s->hwo);
    s->ok = TRUE;
    return s;
}

/* Reclaim any finished buffers so their slots can be reused. */
static void reclaim(waveout_sink *s)
{
    for (int i = 0; i < WO_NUM_BUFFERS; i++)
        if ((s->hdr[i].dwFlags & WHDR_PREPARED) && (s->hdr[i].dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(s->hwo, &s->hdr[i], sizeof(WAVEHDR));
            s->hdr[i].dwFlags = 0;
        }
}

static void push_chunk(waveout_sink *s, const uint8_t *pcm, uint32_t len)
{
    reclaim(s);
    WAVEHDR *h = &s->hdr[s->next];
    if (h->dwFlags & WHDR_PREPARED) {
        /* Ring full: the network briefly outran playback. Drop this chunk
         * rather than block the reader thread. */
        return;
    }
    memcpy(s->buf[s->next], pcm, len);
    ZeroMemory(h, sizeof(*h));
    h->lpData = (LPSTR)s->buf[s->next];
    h->dwBufferLength = len;
    if (waveOutPrepareHeader(s->hwo, h, sizeof(*h)) != MMSYSERR_NOERROR)
        return;
    if (waveOutWrite(s->hwo, h, sizeof(*h)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(s->hwo, h, sizeof(*h));
        h->dwFlags = 0;
        return;
    }
    s->next = (s->next + 1) % WO_NUM_BUFFERS;
    if (!s->started && ++s->queued >= WO_PREBUFFER) {
        waveOutRestart(s->hwo); /* enough prebuffered; begin playback */
        s->started = TRUE;
    }
}

void waveout_feed(waveout_sink *s, const uint8_t *pcm, size_t len)
{
    if (!s || !s->ok)
        return;
    /* Split large chunks across ring buffers. */
    size_t off = 0;
    while (off < len) {
        uint32_t chunk = (uint32_t)((len - off < WO_BUFFER_BYTES)
                                        ? (len - off) : WO_BUFFER_BYTES);
        push_chunk(s, pcm + off, chunk);
        off += chunk;
    }
}

void waveout_reset(waveout_sink *s)
{
    if (!s || !s->ok)
        return;
    /* End of stream: PLAY OUT whatever is buffered, don't discard it. A short
     * sound (fewer than WO_PREBUFFER chunks) never tripped the prebuffer
     * threshold, so start playback now or it would be silently thrown away;
     * longer streams are already playing and simply drain. Calling waveOutReset()
     * here (the old behavior) discarded queued-but-unplayed buffers and clipped
     * the tail of every stream — and made short beeps inaudible entirely. */
    if (!s->started) {
        waveOutRestart(s->hwo);
        s->started = TRUE;
    }
    reclaim(s);
}

void waveout_destroy(waveout_sink *s)
{
    if (!s)
        return;
    if (s->ok) {
        waveOutReset(s->hwo);
        reclaim(s);
        waveOutClose(s->hwo);
    }
    free(s);
}
