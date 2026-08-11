#include "core/client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rfb/rfbclient.h>

#include "core/qemu_audio.h"

#ifdef _WIN32
/* SChannel backend (tls_schannel.c): bytes of already-decrypted plaintext held
 * in the TLS layer that select() on the socket cannot see. 0 for no session. */
unsigned vnc_schannel_pending(rfbClient *client);
#endif

/* Clipboard text from an untrusted server is capped before it reaches the UI. */
#define VNC_MAX_CUT_TEXT (1u << 20) /* 1 MiB */

/* VNC_MAX_CURSOR_DIM lives in client.h (shared with the worker + IPC cap). */

struct vnc_client {
    rfbClient *rfb;
    vnc_client_delegate delegate;
    char *encodings; /* owned copy, or NULL for library default */
    char *ca_file;   /* owned copy, or NULL */
    bool view_only;
    size_t max_fb_bytes; /* 0 = unlimited; else refuse larger framebuffers */
    /* Our copy of the framebuffer pointer (== rfb->frameBuffer). We own that
     * allocation, and rfbClientCleanup() frees the rfbClient WITHOUT freeing
     * the framebuffer — so every path that ends in rfbClientCleanup (including
     * rfbInitClient's internal failure cleanup, where rfb is already gone by
     * the time we regain control) must free it via this pointer. */
    uint8_t *fb;
    qa_state audio_state;
};

/* Tag used to stash our wrapper on the rfbClient for callback bridging. */
static int k_self_tag;

static vnc_client *self_of(rfbClient *rfb)
{
    return (vnc_client *)rfbClientGetClientData(rfb, &k_self_tag);
}

static void emit_log(vnc_client *c, vnc_log_level level, const char *fmt, ...)
{
    if (!c->delegate.on_log)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    c->delegate.on_log(c->delegate.user, level, buf);
}

/* ---- libvncclient callback bridges ------------------------------------- */

static rfbBool cb_malloc_framebuffer(rfbClient *rfb)
{
    vnc_client *c = self_of(rfb);
    const int w = rfb->width;
    const int h = rfb->height;

    if (w <= 0 || h <= 0 || w > VNC_MAX_FB_WIDTH || h > VNC_MAX_FB_HEIGHT) {
        emit_log(c, VNC_LOG_ERROR, "rejecting framebuffer size %dx%d", w, h);
        return FALSE;
    }

    /* 4 bytes per pixel (32bpp). Overflow-checked in size_t; the caps above
     * already guarantee this cannot overflow, but the check is explicit so the
     * invariant survives future cap changes. */
    const size_t bpp = 4;
    size_t pixels = (size_t)w * (size_t)h;
    if (pixels / (size_t)w != (size_t)h)
        return FALSE;
    size_t bytes = pixels * bpp;
    if (bytes / bpp != pixels)
        return FALSE;

    /* Refuse a desktop larger than the embedder's backing store (e.g. the
     * worker's shared framebuffer). Without this a server can pick a size that
     * passes the dimension caps but overflows the store when pixels are copied
     * out. Failing here aborts the RFB handshake — the connection fails closed. */
    if (c->max_fb_bytes && bytes > c->max_fb_bytes) {
        emit_log(c, VNC_LOG_ERROR,
                 "rejecting framebuffer %dx%d (%zu bytes > %zu-byte store)",
                 w, h, bytes, c->max_fb_bytes);
        return FALSE;
    }

    uint8_t *fb = malloc(bytes);
    if (!fb) {
        emit_log(c, VNC_LOG_ERROR, "framebuffer alloc failed (%zu bytes)", bytes);
        return FALSE;
    }
    memset(fb, 0, bytes);

    free(rfb->frameBuffer);
    rfb->frameBuffer = fb;
    c->fb = fb; /* track for the teardown paths (see struct comment) */

    /* Keep the library's notion of the pixel layout consistent with our 32bpp
     * request so decoders write where we expect. */
    rfb->updateRect.x = rfb->updateRect.y = 0;
    rfb->updateRect.w = w;
    rfb->updateRect.h = h;

    if (c->delegate.on_desktop_resize)
        c->delegate.on_desktop_resize(c->delegate.user, w, h);
    return TRUE;
}

static void cb_got_update(rfbClient *rfb, int x, int y, int w, int h)
{
    vnc_client *c = self_of(rfb);

    /* Clamp the server-reported rect to the framebuffer before anyone blits. */
    const int fw = rfb->width, fh = rfb->height;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x > fw || y > fh) return;
    if (x + w > fw) w = fw - x;
    if (y + h > fh) h = fh - y;
    if (w <= 0 || h <= 0) return;

    if (c->delegate.on_framebuffer_update)
        c->delegate.on_framebuffer_update(c->delegate.user, x, y, w, h);
}

static void cb_got_cut_text(rfbClient *rfb, const char *text, int textlen)
{
    vnc_client *c = self_of(rfb);
    if (textlen < 0)
        return;
    size_t len = (size_t)textlen;
    if (len > VNC_MAX_CUT_TEXT)
        len = VNC_MAX_CUT_TEXT;
    if (c->delegate.on_cut_text)
        c->delegate.on_cut_text(c->delegate.user, text, len);
}

static void cb_got_cursor(rfbClient *rfb, int xhot, int yhot,
                          int width, int height, int bytesPerPixel)
{
    vnc_client *c = self_of(rfb);
    if (!c->delegate.on_cursor)
        return;
    if (width <= 0 || height <= 0 ||
        width > VNC_MAX_CURSOR_DIM || height > VNC_MAX_CURSOR_DIM)
        return;
    if (bytesPerPixel != 4)
        return; /* we request 32bpp; ignore anything else */
    if (!rfb->rcSource)
        return;
    c->delegate.on_cursor(c->delegate.user, xhot, yhot, width, height,
                          rfb->rcSource, rfb->rcMask);
}

static char *cb_get_password(rfbClient *rfb)
{
    vnc_client *c = self_of(rfb);
    if (!c->delegate.get_password)
        return NULL;
    /* A configured CA means the user REQUIRES authenticated TLS. If we are asked
     * for a VNC-auth password while no TLS session is active, the server has
     * downgraded the transport (e.g. by announcing RFB 3.3, whose legacy security
     * path bypasses the VeNCrypt pin in SetClientAuthSchemes) and the DES response
     * would travel in CLEARTEXT — offline-crackable. Refuse: returning NULL aborts
     * the authentication and the connection, before the password leaves the host.
     *
     * NOTE on the signal: on the SHIPPED SChannel backend `tlsSession != NULL`
     * means X509-verified TLS (it refuses anonymous/non-X509 VeNCrypt), so this is
     * exact. The GnuTLS *reference* backend (Linux test only, never shipped) is
     * permissive and sets tlsSession for anonymous TLS too, so on that build the
     * authoritative anon defense is SChannel's refusal, not this guard. */
    if (c->ca_file && !rfb->tlsSession) {
        emit_log(c, VNC_LOG_ERROR,
                 "refusing to send a password over a non-TLS transport "
                 "(a CA is set; the server may be attempting a downgrade)");
        return NULL;
    }
    return c->delegate.get_password(c->delegate.user);
}

/* Supply credentials for VeNCrypt. For X509 we return the CA bundle so the
 * backend can VERIFY the server certificate; with no CA configured we return
 * NULL, which makes the handshake fail closed (no unverified TLS). */
static rfbCredential *cb_get_credential(rfbClient *rfb, int type)
{
    vnc_client *c = self_of(rfb);
    if (type == rfbCredentialTypeX509 && c->ca_file) {
        rfbCredential *cred = calloc(1, sizeof(*cred));
        if (!cred)
            return NULL;
        cred->x509Credential.x509CACertFile = strdup(c->ca_file);
        cred->x509Credential.x509CrlVerifyMode = 0; /* rfbX509CrlVerifyNone */
        return cred; /* libvncclient frees it */
    }
    /* VeNCrypt Plain (username/password) is not supported; QEMU uses X509 +
     * optional VNC auth (handled via GetPassword). */
    return NULL;
}

static void cb_led_state(rfbClient *rfb, int value, int pad)
{
    (void)pad;
    vnc_client *c = self_of(rfb);
    if (c->delegate.on_led)
        c->delegate.on_led(c->delegate.user, (uint8_t)(value & 0xFF));
}

/* ---- QEMU audio extension glue ----------------------------------------- */

static void audio_end_cb(void *user)
{
    vnc_client *c = user;
    if (c->delegate.on_audio_end)
        c->delegate.on_audio_end(c->delegate.user);
}
static void audio_data_cb(void *user, const uint8_t *pcm, uint32_t len)
{
    vnc_client *c = user;
    if (c->delegate.on_audio_data)
        c->delegate.on_audio_data(c->delegate.user, pcm, len);
}

static bool audio_read(void *ctx, void *dst, uint32_t n)
{
    return ReadFromRFBServer((rfbClient *)ctx, (char *)dst, n) == TRUE;
}

/* Called by libvncclient for server message types it does not handle itself. */
static rfbBool audio_handle_message(rfbClient *rfb, rfbServerToClientMsg *msg)
{
    if (msg->type != rfbQemuEvent) /* 255 */
        return FALSE;
    vnc_client *c = self_of(rfb);
    if (!c)
        return FALSE;
    qa_sink sink = { c, NULL, audio_end_cb, audio_data_cb };
    return qemu_audio_parse_server_msg(&c->audio_state, audio_read, rfb, &sink)
               ? TRUE : FALSE;
}

/* Advertised so the server knows we can receive audio. Registered process-wide
 * once; harmless on non-QEMU servers (they ignore the pseudo-encoding and we
 * never send an ENABLE unless the user opts in). */
static int audio_encodings[] = { (int)VNC_ENCODING_QEMU_AUDIO, 0 };
static rfbClientProtocolExtension audio_extension = {
    audio_encodings,     /* encodings (zero-terminated) */
    NULL,                /* handleEncoding (audio is delivered via msg 255) */
    audio_handle_message,/* handleMessage */
    NULL, NULL, NULL
};

/* ---- global logger (rfbClientLog/Err are process-global variadic hooks) --- */

/* libvncclient logs through the process-global rfbClientLog/Err (no client
 * context). We route them to an optional sink so the sandboxed worker can
 * forward the protocol/TLS timeline to the diagnostic log instead of losing it
 * to a null stderr. Falls back to stderr when no sink is set (headless test). */
static void (*g_log_fn)(void *, vnc_log_level, const char *);
static void *g_log_user;

void vnc_client_set_global_log(void (*fn)(void *, vnc_log_level, const char *),
                               void *user)
{
    g_log_fn = fn;
    g_log_user = user;
}

static void emit_global(vnc_log_level lvl, const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0)
        return;
    size_t len = strnlen(buf, sizeof(buf));
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    if (len == 0)
        return;
    if (g_log_fn)
        g_log_fn(g_log_user, lvl, buf);
    else
        fprintf(stderr, "%s\n", buf);
}

static void global_log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); emit_global(VNC_LOG_INFO, fmt, ap); va_end(ap);
}
static void global_log_err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); emit_global(VNC_LOG_ERROR, fmt, ap); va_end(ap);
}

/* ---- public API -------------------------------------------------------- */

vnc_client *vnc_client_create(const vnc_client_delegate *delegate)
{
    vnc_client *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->delegate = *delegate;

    rfbClientLog = global_log_info;
    rfbClientErr = global_log_err;

    /* Register the QEMU audio extension once for the process. */
    static bool audio_registered = false;
    if (!audio_registered) {
        rfbClientRegisterExtension(&audio_extension);
        audio_registered = true;
    }

    /* 8 bits/sample, 3 samples/pixel, 4 bytes/pixel => 32bpp true colour. */
    c->rfb = rfbGetClient(8, 3, 4);
    if (!c->rfb) {
        free(c);
        return NULL;
    }

    /* Request B,G,R,X byte order (blue in the low bits) instead of the library's
     * little-endian default of R,G,B,X. This is what a Win32 BI_RGB 32bpp DIB
     * expects, so StretchDIBits blits with no per-pixel swap, and it matches
     * png_write_bgrx() — the whole pixel pipeline (shm, DIB, PNG) then agrees on
     * one byte order. rfbInitClient sends this to the server via SetPixelFormat,
     * and the server is required to deliver pixels in the requested format. */
    c->rfb->format.redShift = 16;
    c->rfb->format.greenShift = 8;
    c->rfb->format.blueShift = 0;

    /* Default to the decoders actually compiled in (Tight needs libjpeg, added
     * later). Callers may override via vnc_client_set_encodings. */
    c->encodings = strdup("copyrect zrle hextile zlib corre rre trle ultra raw");

    rfbClientSetClientData(c->rfb, &k_self_tag, c);
    c->rfb->MallocFrameBuffer = cb_malloc_framebuffer;
    c->rfb->GotFrameBufferUpdate = cb_got_update;
    c->rfb->GotXCutText = cb_got_cut_text;
    /* Setting GotXCutTextUTF8 is what makes libvncclient negotiate the Extended
     * Clipboard (what QEMU speaks); it shares the classic path's capping and
     * forwarding since both deliver a length-counted byte run. */
    c->rfb->GotXCutTextUTF8 = cb_got_cut_text;
    c->rfb->GotCursorShape = cb_got_cursor;
    c->rfb->HandleKeyboardLedState = cb_led_state;
    c->rfb->appData.useRemoteCursor = TRUE; /* request cursor pseudo-encodings */
    c->rfb->GetPassword = cb_get_password;
    c->rfb->GetCredential = cb_get_credential;
    c->rfb->canHandleNewFBSize = TRUE;
    return c;
}

void vnc_client_set_encodings(vnc_client *c, const char *encodings)
{
    free(c->encodings);
    c->encodings = encodings ? strdup(encodings) : NULL;
}

void vnc_client_set_view_only(vnc_client *c, bool view_only)
{
    c->view_only = view_only;
}

void vnc_client_set_max_framebuffer_bytes(vnc_client *c, size_t max_bytes)
{
    c->max_fb_bytes = max_bytes;
}

void vnc_client_set_ca_file(vnc_client *c, const char *ca_file)
{
    free(c->ca_file);
    c->ca_file = ca_file ? strdup(ca_file) : NULL;
}

bool vnc_client_connect(vnc_client *c, const char *host, int port)
{
    /* libvncclient initialised serverHost to strdup(""); free that before we
     * replace it. It owns the new storage and frees it in cleanup. */
    free(c->rfb->serverHost);
    c->rfb->serverHost = strdup(host);
    c->rfb->serverPort = port;
    c->rfb->appData.viewOnly = c->view_only ? TRUE : FALSE;
    if (c->encodings)
        c->rfb->appData.encodingsString = c->encodings;

    /* If the user supplied a CA they are asking for authenticated, encrypted
     * transport. libvncclient otherwise accepts the FIRST security type the
     * server offers, so a hostile server or a MITM could offer None/VncAuth
     * ahead of VeNCrypt and downgrade the whole session to CLEARTEXT — framebuffer
     * and every forwarded keystroke (including passwords typed into the guest) in
     * the clear — without ever entering the (correctly fail-closed) TLS path.
     * Restrict the accepted security types to VeNCrypt so a downgrade cannot be
     * negotiated: if the server does not offer VeNCrypt there is no common type
     * and the handshake fails closed. The SChannel/GnuTLS backend then further
     * restricts VeNCrypt to X509 subtypes and verifies the certificate against
     * this CA. (SetClientAuthSchemes copies the list; the library frees it.) */
    if (c->ca_file) {
        const uint32_t require_vencrypt[] = { rfbVeNCrypt };
        SetClientAuthSchemes(c->rfb, require_vencrypt, 1);
    }

    /* Pass no argv so nothing is parsed from the command line. */
    int argc = 0;
    if (!rfbInitClient(c->rfb, &argc, NULL)) {
        /* rfbInitClient frees the client on failure; drop our dangling pointer.
         * It does NOT free the framebuffer (app-owned), which may already have
         * been allocated if initialisation failed after MallocFrameBuffer. */
        c->rfb = NULL;
        free(c->fb);
        c->fb = NULL;
        return false;
    }
    /* Final downgrade guard: if the user required TLS (CA set) but no TLS session
     * is active, the whole handshake ran in cleartext — e.g. an RFB 3.3 downgrade
     * that offered "None" (no password callback fires, so cb_get_password's guard
     * never runs). rfbInitClient has by now sent SetPixelFormat/SetEncodings and
     * one FramebufferUpdateRequest in the clear (none of which carry secrets); tear
     * the session down NOW — before the pump loop runs — so no framebuffer pixels
     * are ever displayed and no keystrokes/clipboard ever traverse the cleartext
     * link. (On SChannel this is a backstop; the primary block is the anon refusal
     * plus SetClientAuthSchemes above.) */
    if (c->ca_file && !c->rfb->tlsSession) {
        emit_log(c, VNC_LOG_ERROR,
                 "connection refused: a CA is configured but the transport is not "
                 "TLS (possible protocol-version/security downgrade)");
        rfbClientCleanup(c->rfb); /* frees the client, not the framebuffer */
        c->rfb = NULL;
        free(c->fb);
        c->fb = NULL;
        return false;
    }
    return true;
}

/* True if a whole message is already sitting in a buffer that select() cannot
 * see, so the caller must NOT block on the socket. libvncclient reads a chunk
 * (up to RFB_BUFFER_SIZE) at a time, so several coalesced RFB messages routinely
 * land in client->buffered after the first is processed; over TLS the SChannel
 * backend can additionally hold decrypted plaintext from an oversized record.
 * Without this check a trailing server-pushed message (ServerCutText, Bell,
 * audio DATA) with no follow-on socket traffic would sit unprocessed until the
 * next unrelated read. */
static bool has_buffered_input(vnc_client *c)
{
    if (c->rfb->buffered > 0)
        return true;
#ifdef _WIN32
    if (vnc_schannel_pending(c->rfb) > 0) /* decrypted-but-unconsumed plaintext */
        return true;
#endif
    return false;
}

int vnc_client_pump(vnc_client *c, unsigned timeout_us)
{
    if (!c->rfb)
        return -1;
    if (!has_buffered_input(c)) {
        int n = WaitForMessage(c->rfb, timeout_us);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
    }
    if (!HandleRFBServerMessage(c->rfb))
        return -1;
    return 1;
}

int vnc_client_wait(vnc_client *c, unsigned timeout_us)
{
    if (!c->rfb)
        return -1;
    if (has_buffered_input(c))
        return 1;
    return WaitForMessage(c->rfb, timeout_us);
}

int vnc_client_handle_message(vnc_client *c)
{
    if (!c->rfb)
        return -1;
    return HandleRFBServerMessage(c->rfb) ? 0 : -1;
}

bool vnc_client_request_update(vnc_client *c, bool incremental)
{
    if (!c->rfb)
        return false;
    return SendFramebufferUpdateRequest(c->rfb, 0, 0, c->rfb->width,
                                        c->rfb->height,
                                        incremental ? TRUE : FALSE) == TRUE;
}

bool vnc_client_request_desktop_size(vnc_client *c, int width, int height)
{
    /* A desktop resize mutates the remote server, so honor view-only exactly like
     * the key/pointer/cut-text senders do — a "view only (no input sent)" session
     * must not change the server's geometry. */
    if (!c->rfb || c->view_only || width <= 0 || height <= 0 ||
        width > VNC_MAX_FB_WIDTH || height > VNC_MAX_FB_HEIGHT)
        return false;
    /* SendExtDesktopSize is a no-op until the server has advertised its screen
     * geometry (i.e. it supports ExtendedDesktopSize, which we advertise because
     * canHandleNewFBSize is set). It also skips the send when the size already
     * matches, so this is safe to call repeatedly (e.g. on every window resize). */
    return SendExtDesktopSize(c->rfb, (uint16_t)width, (uint16_t)height) == TRUE;
}

bool vnc_client_send_key(vnc_client *c, uint32_t keysym, bool down)
{
    if (!c->rfb || c->view_only)
        return false;
    return SendKeyEvent(c->rfb, keysym, down ? TRUE : FALSE) == TRUE;
}

bool vnc_client_send_key_ext(vnc_client *c, uint32_t keysym, uint32_t keycode,
                             bool down)
{
    if (!c->rfb || c->view_only)
        return false;
    /* SendExtendedKeyEvent returns FALSE if the server never negotiated the
     * QEMU extension; fall back to a keysym-only event in that case. */
    if (SendExtendedKeyEvent(c->rfb, keysym, keycode, down ? TRUE : FALSE))
        return true;
    if (keysym)
        return SendKeyEvent(c->rfb, keysym, down ? TRUE : FALSE) == TRUE;
    return false;
}

bool vnc_client_send_pointer(vnc_client *c, int x, int y, int button_mask)
{
    if (!c->rfb || c->view_only)
        return false;
    return SendPointerEvent(c->rfb, x, y, button_mask) == TRUE;
}

bool vnc_client_send_cut_text(vnc_client *c, const char *text, size_t len)
{
    if (!c->rfb || c->view_only)
        return false;
    if (len > VNC_MAX_CUT_TEXT)
        len = VNC_MAX_CUT_TEXT;
    /* Prefer the Extended-Clipboard (UTF-8) path; libvncclient falls back to
     * classic cut text automatically if the server lacks the capability.
     * libvncclient takes a non-const char*; it does not modify the buffer. */
    return SendClientCutTextUTF8(c->rfb, (char *)text, (int)len) == TRUE;
}

vnc_handle vnc_client_socket(const vnc_client *c)
{
    if (!c->rfb)
        return (vnc_handle)-1;
    return (vnc_handle)c->rfb->sock;
}

bool vnc_client_audio_enable(vnc_client *c, uint8_t format, uint8_t channels,
                             uint32_t frequency)
{
    if (!c->rfb)
        return false;
    /* Enforce the documented format bounds at the point we build SET_FORMAT, so
     * the API is self-protecting regardless of caller (the worker also validates
     * the untrusted VNC_CMD_AUDIO_ENABLE before reaching here — defense in depth). */
    if (format > QA_FORMAT_S32 ||
        channels < 1 || channels > QA_MAX_CHANNELS ||
        frequency < QA_MIN_FREQ || frequency > QA_MAX_FREQ) {
        emit_log(c, VNC_LOG_WARN,
                 "audio enable rejected: fmt %u, %u ch, %u Hz out of range",
                 format, channels, frequency);
        return false;
    }
    uint8_t msg[10];
    size_t len = 0;
    qemu_audio_build_set_format(msg, &len, format, channels, frequency);
    if (WriteToRFBServer(c->rfb, (const char *)msg, (unsigned)len) != TRUE)
        return false;
    qemu_audio_build_enable(msg, &len);
    return WriteToRFBServer(c->rfb, (const char *)msg, (unsigned)len) == TRUE;
}

bool vnc_client_audio_disable(vnc_client *c)
{
    if (!c->rfb)
        return false;
    uint8_t msg[10];
    size_t len = 0;
    qemu_audio_build_disable(msg, &len);
    return WriteToRFBServer(c->rfb, (const char *)msg, (unsigned)len) == TRUE;
}

const uint8_t *vnc_client_framebuffer(const vnc_client *c)
{
    return c->rfb ? c->rfb->frameBuffer : NULL;
}

int vnc_client_width(const vnc_client *c)
{
    return c->rfb ? c->rfb->width : 0;
}

int vnc_client_height(const vnc_client *c)
{
    return c->rfb ? c->rfb->height : 0;
}

const char *vnc_client_desktop_name(const vnc_client *c)
{
    return c->rfb ? c->rfb->desktopName : NULL;
}

void vnc_client_destroy(vnc_client *c)
{
    if (!c)
        return;
    if (c->rfb) {
        c->rfb->frameBuffer = NULL; /* we free via c->fb below */
        rfbClientCleanup(c->rfb);
    }
    free(c->fb);
    free(c->encodings);
    free(c->ca_file);
    free(c);
}
