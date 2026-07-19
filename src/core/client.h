/*
 * vnc-lightweight-x64 — portable client core (layer: src/core, no Win32).
 *
 * Thin, allocation-disciplined wrapper over vendored libvncclient. Owns the
 * socket-facing RFB state and enforces the trust-boundary validation caps from
 * the project security model (every server byte is untrusted). Knows nothing
 * about GDI, waveOut, or any UI toolkit — those live in src/app and receive
 * decoded data through the delegate below.
 */
#ifndef VNC_CORE_CLIENT_H
#define VNC_CORE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hard caps on server-controlled dimensions, applied before any allocation or
 * blit. 16384 is far above any real desktop and keeps w*h*4 well inside size_t. */
#define VNC_MAX_FB_WIDTH  16384
#define VNC_MAX_FB_HEIGHT 16384

typedef enum {
    VNC_LOG_ERROR = 0,
    VNC_LOG_WARN,
    VNC_LOG_INFO,
    VNC_LOG_DEBUG
} vnc_log_level;

/*
 * Delegate: the core calls these; the embedder (headless test, or the Win32
 * worker) supplies them. All callbacks receive the embedder's `user` pointer.
 * The framebuffer passed to on_framebuffer_update is 32bpp little-endian, owned
 * by the core, valid only for the lifetime of the client.
 */
typedef struct {
    void *user;

    /* A rectangle of the framebuffer changed. (x,y,w,h) are already clamped to
     * the current framebuffer bounds by the core. */
    void (*on_framebuffer_update)(void *user, int x, int y, int w, int h);

    /* Framebuffer (re)allocated to width x height (e.g. DesktopSize). Called
     * before the first update and on every server-driven resize. */
    void (*on_desktop_resize)(void *user, int width, int height);

    /* Server cut text (clipboard). `text` is not NUL-guaranteed; `len` is the
     * authoritative length and is already capped by the core. Fires for both
     * classic and Extended-Clipboard (UTF-8) server text. */
    void (*on_cut_text)(void *user, const char *text, size_t len);

    /* Server cursor shape (client-side cursor). `bgra` is width*height 32bpp
     * pixels; `mask` is width*height bytes (non-zero = opaque). Both owned by
     * the core, valid only during the call. Dimensions are already capped. */
    void (*on_cursor)(void *user, int xhot, int yhot, int width, int height,
                      const uint8_t *bgra, const uint8_t *mask);

    /* Server keyboard LED state changed (QEMU LED State pseudo-encoding).
     * `state` bit0=Scroll, bit1=Num, bit2=Caps lock. */
    void (*on_led)(void *user, uint8_t state);

    /* QEMU Audio extension. begin/end bracket a playback stream; data delivers
     * PCM in the format the client requested (see vnc_client_audio_enable). */
    void (*on_audio_begin)(void *user);
    void (*on_audio_data)(void *user, const uint8_t *pcm, size_t len);
    void (*on_audio_end)(void *user);

    /* Structured log line. Never carries secrets. */
    void (*on_log)(void *user, vnc_log_level level, const char *msg);

    /* Return a heap-allocated password string (freed by libvncclient), or NULL
     * to abort auth. Never sourced from argv. */
    char *(*get_password)(void *user);
} vnc_client_delegate;

typedef struct vnc_client vnc_client;

/* Create a client requesting 32bpp true colour. Returns NULL on OOM. */
vnc_client *vnc_client_create(const vnc_client_delegate *delegate);

/* Route libvncclient's process-global log (rfbClientLog/Err) to a sink instead
 * of stderr, so a sandboxed embedder can forward the protocol/TLS timeline for
 * diagnostics. Pass NULL to restore the stderr default. Process-global. */
void vnc_client_set_global_log(void (*fn)(void *, vnc_log_level, const char *),
                               void *user);

/* Override the negotiated encodings (space-separated, libvncclient syntax),
 * e.g. "zrle hextile copyrect raw". Must be called before vnc_client_connect.
 * The string is copied. */
void vnc_client_set_encodings(vnc_client *c, const char *encodings);

/* Path to a PEM CA bundle used to verify the server certificate for VeNCrypt
 * X509 (the best QEMU-supported auth). Required for an X509 handshake to
 * succeed — with no CA set, the TLS backend refuses to proceed (no
 * trust-on-first-use). The string is copied. */
void vnc_client_set_ca_file(vnc_client *c, const char *ca_file);

/* Set view-only (never send input). */
void vnc_client_set_view_only(vnc_client *c, bool view_only);

/* Upper bound (bytes) on the framebuffer the core will allocate for a server-
 * announced desktop size. An embedder that copies decoded pixels into a fixed
 * backing store (e.g. the worker's shared-memory framebuffer) MUST set this to
 * that store's capacity: a hostile server can otherwise announce a resolution
 * that passes the dimension caps yet is far larger than the store, and the
 * subsequent pixel copy would write out of bounds. When the requested size
 * exceeds this, cb_malloc_framebuffer refuses and the connection fails closed.
 * 0 (default) means "only the VNC_MAX_FB_* dimension caps apply" (the headless
 * tool owns the buffer it reads, so it needs no store limit). */
void vnc_client_set_max_framebuffer_bytes(vnc_client *c, size_t max_bytes);

/* Connect + full RFB handshake to host:port. Returns false on failure. */
bool vnc_client_connect(vnc_client *c, const char *host, int port);

/* Wait up to timeout_us microseconds for server data, then process one batch of
 * server messages. Returns:  1 processed, 0 timed out (nothing to do),
 * -1 on error/disconnect. Single-threaded callers only (see wait/handle). */
int vnc_client_pump(vnc_client *c, unsigned timeout_us);

/* Split form of pump() for multi-threaded callers: wait UNLOCKED for data, then
 * handle the message UNDER THE SAME LOCK used for input sends, so a TLS read and
 * a TLS write never touch the session concurrently. wait() returns >0 ready,
 * 0 timeout, <0 error; handle() returns 0 ok, -1 error/disconnect. */
int vnc_client_wait(vnc_client *c, unsigned timeout_us);
int vnc_client_handle_message(vnc_client *c);

/* Request a framebuffer update (incremental or full). */
bool vnc_client_request_update(vnc_client *c, bool incremental);

/* Input to the server. Safe no-ops in view-only mode. */
bool vnc_client_send_key(vnc_client *c, uint32_t keysym, bool down);
/* Prefer the QEMU Extended Key Event (keysym + XT keycode) when the server
 * negotiated it; otherwise fall back to a plain key event using keysym. */
bool vnc_client_send_key_ext(vnc_client *c, uint32_t keysym, uint32_t keycode,
                             bool down);
bool vnc_client_send_pointer(vnc_client *c, int x, int y, int button_mask);
bool vnc_client_send_cut_text(vnc_client *c, const char *text, size_t len);

/* Underlying RFB socket, for event loops that select() on it alongside other
 * handles. Returns (vnc_handle)-1 pre-connect. */
typedef intptr_t vnc_handle;
vnc_handle vnc_client_socket(const vnc_client *c);

/* QEMU Audio: request the server start/stop streaming audio in the given format
 * (QA_FORMAT_* / channels / frequency from qemu_audio.h). Only meaningful
 * against a QEMU server; harmless-but-useless elsewhere, so callers gate this on
 * user opt-in. Must hold the same lock as other sends. */
bool vnc_client_audio_enable(vnc_client *c, uint8_t format, uint8_t channels,
                             uint32_t frequency);
bool vnc_client_audio_disable(vnc_client *c);

/* Accessors for the current framebuffer (32bpp LE). Returns NULL/0 pre-connect. */
const uint8_t *vnc_client_framebuffer(const vnc_client *c);
int vnc_client_width(const vnc_client *c);
int vnc_client_height(const vnc_client *c);

/* Human-readable server name reported at handshake (may be NULL). */
const char *vnc_client_desktop_name(const vnc_client *c);

void vnc_client_destroy(vnc_client *c);

#endif /* VNC_CORE_CLIENT_H */
