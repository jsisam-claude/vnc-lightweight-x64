#include "core/client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rfb/rfbclient.h>

#include "core/qemu_audio.h"

/* Clipboard text from an untrusted server is capped before it reaches the UI. */
#define VNC_MAX_CUT_TEXT (1u << 20) /* 1 MiB */

/* Client-side cursors above this in either dimension are ignored (matches the
 * EVT_CURSOR IPC cap; also bounds work per cursor update). */
#define VNC_MAX_CURSOR_DIM 256

struct vnc_client {
    rfbClient *rfb;
    vnc_client_delegate delegate;
    char *encodings; /* owned copy, or NULL for library default */
    bool view_only;
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

    uint8_t *fb = malloc(bytes);
    if (!fb) {
        emit_log(c, VNC_LOG_ERROR, "framebuffer alloc failed (%zu bytes)", bytes);
        return FALSE;
    }
    memset(fb, 0, bytes);

    free(rfb->frameBuffer);
    rfb->frameBuffer = fb;

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

static void cb_got_cut_text_utf8(rfbClient *rfb, const char *text, int textlen)
{
    /* Extended-Clipboard (UTF-8) path; same capping/forwarding as classic. */
    cb_got_cut_text(rfb, text, textlen);
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
    return c->delegate.get_password(c->delegate.user);
}

static void cb_led_state(rfbClient *rfb, int value, int pad)
{
    (void)pad;
    vnc_client *c = self_of(rfb);
    if (c->delegate.on_led)
        c->delegate.on_led(c->delegate.user, (uint8_t)(value & 0xFF));
}

/* ---- QEMU audio extension glue ----------------------------------------- */

static void audio_begin_cb(void *user)
{
    vnc_client *c = user;
    if (c->delegate.on_audio_begin)
        c->delegate.on_audio_begin(c->delegate.user);
}
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
    qa_sink sink = { c, audio_begin_cb, audio_end_cb, audio_data_cb };
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

static void global_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- public API -------------------------------------------------------- */

vnc_client *vnc_client_create(const vnc_client_delegate *delegate)
{
    vnc_client *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->delegate = *delegate;

    rfbClientLog = global_log;
    rfbClientErr = global_log;

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

    /* Default to the decoders actually compiled in (Tight needs libjpeg, added
     * later). Callers may override via vnc_client_set_encodings. */
    c->encodings = strdup("copyrect zrle hextile zlib corre rre trle ultra raw");

    rfbClientSetClientData(c->rfb, &k_self_tag, c);
    c->rfb->MallocFrameBuffer = cb_malloc_framebuffer;
    c->rfb->GotFrameBufferUpdate = cb_got_update;
    c->rfb->GotXCutText = cb_got_cut_text;
    /* Setting GotXCutTextUTF8 is what makes libvncclient negotiate the Extended
     * Clipboard (what QEMU speaks). */
    c->rfb->GotXCutTextUTF8 = cb_got_cut_text_utf8;
    c->rfb->GotCursorShape = cb_got_cursor;
    c->rfb->HandleKeyboardLedState = cb_led_state;
    c->rfb->appData.useRemoteCursor = TRUE; /* request cursor pseudo-encodings */
    c->rfb->GetPassword = cb_get_password;
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

    /* Pass no argv so nothing is parsed from the command line. */
    int argc = 0;
    if (!rfbInitClient(c->rfb, &argc, NULL)) {
        /* rfbInitClient frees the client on failure; drop our dangling pointer. */
        c->rfb = NULL;
        return false;
    }
    return true;
}

int vnc_client_pump(vnc_client *c, unsigned timeout_us)
{
    if (!c->rfb)
        return -1;
    int n = WaitForMessage(c->rfb, timeout_us);
    if (n < 0)
        return -1;
    if (n == 0)
        return 0;
    if (!HandleRFBServerMessage(c->rfb))
        return -1;
    return 1;
}

bool vnc_client_request_update(vnc_client *c, bool incremental)
{
    if (!c->rfb)
        return false;
    return SendFramebufferUpdateRequest(c->rfb, 0, 0, c->rfb->width,
                                        c->rfb->height,
                                        incremental ? TRUE : FALSE) == TRUE;
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
        free(c->rfb->frameBuffer);
        c->rfb->frameBuffer = NULL;
        rfbClientCleanup(c->rfb);
    }
    free(c->encodings);
    free(c);
}
