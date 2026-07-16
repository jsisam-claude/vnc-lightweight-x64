/*
 * audio_waveout — Windows waveOut playback sink for the QEMU audio stream.
 *
 * Lives in the trusted UI process. The reader thread owns it (audio events are
 * handled there, off the GUI thread). A small ring of WAVEHDR buffers is
 * prebuffered before playback starts to absorb network jitter; if the ring
 * fills (the network briefly outruns playback) the oldest pending chunk is
 * dropped rather than blocking.
 */
#ifndef VNC_APP_AUDIO_WAVEOUT_H
#define VNC_APP_AUDIO_WAVEOUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct waveout_sink waveout_sink;

/* Create a sink for the given QEMU audio format (QA_FORMAT_* from qemu_audio.h),
 * channel count, and sample rate. Returns NULL on failure. */
waveout_sink *waveout_create(uint8_t qa_format, uint8_t channels, uint32_t freq);

/* Queue PCM for playback. Safe to call repeatedly from the reader thread. */
void waveout_feed(waveout_sink *s, const uint8_t *pcm, size_t len);

/* Stop playback and reset (e.g. on AUDIO_END); the sink can be fed again. */
void waveout_reset(waveout_sink *s);

void waveout_destroy(waveout_sink *s);

#endif /* VNC_APP_AUDIO_WAVEOUT_H */
