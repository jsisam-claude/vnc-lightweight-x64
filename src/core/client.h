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
     * authoritative length and is already capped by the core. */
    void (*on_cut_text)(void *user, const char *text, size_t len);

    /* Structured log line. Never carries secrets. */
    void (*on_log)(void *user, vnc_log_level level, const char *msg);

    /* Return a heap-allocated password string (freed by libvncclient), or NULL
     * to abort auth. Never sourced from argv. */
    char *(*get_password)(void *user);
} vnc_client_delegate;

typedef struct vnc_client vnc_client;

/* Create a client requesting 32bpp true colour. Returns NULL on OOM. */
vnc_client *vnc_client_create(const vnc_client_delegate *delegate);

/* Override the negotiated encodings (space-separated, libvncclient syntax),
 * e.g. "zrle hextile copyrect raw". Must be called before vnc_client_connect.
 * The string is copied. */
void vnc_client_set_encodings(vnc_client *c, const char *encodings);

/* Set view-only (never send input). */
void vnc_client_set_view_only(vnc_client *c, bool view_only);

/* Connect + full RFB handshake to host:port. Returns false on failure. */
bool vnc_client_connect(vnc_client *c, const char *host, int port);

/* Wait up to timeout_us microseconds for server data, then process one batch of
 * server messages. Returns:  1 processed, 0 timed out (nothing to do),
 * -1 on error/disconnect. */
int vnc_client_pump(vnc_client *c, unsigned timeout_us);

/* Request a framebuffer update (incremental or full). */
bool vnc_client_request_update(vnc_client *c, bool incremental);

/* Input to the server. Safe no-ops in view-only mode. */
bool vnc_client_send_key(vnc_client *c, uint32_t keysym, bool down);
bool vnc_client_send_pointer(vnc_client *c, int x, int y, int button_mask);
bool vnc_client_send_cut_text(vnc_client *c, const char *text, size_t len);

/* Underlying RFB socket, for event loops that select() on it alongside other
 * handles. Returns (vnc_handle)-1 pre-connect. */
typedef intptr_t vnc_handle;
vnc_handle vnc_client_socket(const vnc_client *c);

/* Accessors for the current framebuffer (32bpp LE). Returns NULL/0 pre-connect. */
const uint8_t *vnc_client_framebuffer(const vnc_client *c);
int vnc_client_width(const vnc_client *c);
int vnc_client_height(const vnc_client *c);

/* Human-readable server name reported at handshake (may be NULL). */
const char *vnc_client_desktop_name(const vnc_client *c);

void vnc_client_destroy(vnc_client *c);

#endif /* VNC_CORE_CLIENT_H */
