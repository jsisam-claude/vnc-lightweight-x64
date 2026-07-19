/*
 * vncworker — the untrusted decoder process.
 *
 * Owns the TCP socket and runs all RFB parsing (vendored libvncclient) inside
 * the OS sandbox set up by the UI (AppContainer on Windows). It never touches
 * the filesystem, registry, UI, or any network endpoint other than its one
 * server socket. Decoded pixels go into the shared framebuffer; everything else
 * (dirty rects, resize, clipboard, later audio/LED) travels as fixed IPC
 * messages. Input commands arrive from the UI and are forwarded to the server.
 *
 * Threading: the main thread pumps the RFB socket; a reader thread consumes UI
 * commands. A single mutex serialises all libvncclient API calls so socket
 * writes never interleave. The channel itself is single-writer (main thread
 * emits events) and single-reader (reader thread consumes commands), so it
 * needs no lock of its own.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/modes.h"
#include "core/client.h"
#include "core/qemu_audio.h"
#include "ipc/channel.h"
#include "ipc/protocol.h"
#include "ipc/shm.h"

/* Audio format we request from QEMU: signed 16-bit, stereo, 44.1 kHz. */
#define WORKER_AUDIO_FORMAT   QA_FORMAT_S16
#define WORKER_AUDIO_CHANNELS 2
#define WORKER_AUDIO_FREQ     44100u

/* Zero memory so the compiler cannot optimise it away (portable). */
static void secure_wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--)
        *v++ = 0;
}

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
typedef CRITICAL_SECTION worker_mutex;
typedef HANDLE           worker_thread;
static void mtx_init(worker_mutex *m)   { InitializeCriticalSection(m); }
static void mtx_lock(worker_mutex *m)   { EnterCriticalSection(m); }
static void mtx_unlock(worker_mutex *m) { LeaveCriticalSection(m); }
#else
#  include <pthread.h>
#  include <signal.h>
#  include <sys/select.h>
typedef pthread_mutex_t worker_mutex;
typedef pthread_t       worker_thread;
static void mtx_init(worker_mutex *m)   { pthread_mutex_init(m, NULL); }
static void mtx_lock(worker_mutex *m)   { pthread_mutex_lock(m); }
static void mtx_unlock(worker_mutex *m) { pthread_mutex_unlock(m); }
#endif

typedef struct {
    vnc_channel   ch;
    vnc_shm      *shm;
    size_t        shm_capacity;
    vnc_client   *client;
    worker_mutex  api_lock;   /* serialises libvncclient calls */
    worker_mutex  log_lock;   /* serialises channel writes from the log sink */
    volatile int  running;
    bool          want_audio;
} worker;

/* ---- delegate: core -> IPC --------------------------------------------- */

static void w_on_resize(void *user, int width, int height)
{
    worker *w = user;
    size_t need = (size_t)width * (size_t)height * 4u;
    if (need > w->shm_capacity) {
        /* Shouldn't happen: the mapping is sized for VNC_MAX_FB_*. Fail closed. */
        vnc_ipc_status st = { VNC_STATUS_DISCONNECTED };
        vnc_channel_send(&w->ch, VNC_EVT_STATUS, &st, sizeof(st));
        w->running = 0;
        return;
    }
    vnc_shm_header *h = vnc_shm_hdr(w->shm);
    h->width = (uint32_t)width;
    h->height = (uint32_t)height;
    h->generation++;
    vnc_ipc_resize r = { (uint32_t)width, (uint32_t)height };
    vnc_channel_send(&w->ch, VNC_EVT_RESIZE, &r, sizeof(r));
}

static void w_on_update(void *user, int x, int y, int wdt, int hgt)
{
    worker *w = user;
    const uint8_t *fb = vnc_client_framebuffer(w->client);
    int fbw = vnc_client_width(w->client);
    if (!fb || fbw <= 0)
        return;

    uint8_t *dst = vnc_shm_pixels(w->shm);
    size_t cap = w->shm_capacity;
    for (int row = 0; row < hgt; row++) {
        size_t off = ((size_t)(y + row) * (size_t)fbw + (size_t)x) * 4u;
        size_t rb = (size_t)wdt * 4u;
        /* Defense in depth: the framebuffer size is already capped to the shm at
         * allocation, but never let a copy run past the mapping regardless. */
        if (off > cap || rb > cap - off)
            break;
        memcpy(dst + off, fb + off, rb);
    }
    vnc_ipc_rect r = { (uint16_t)x, (uint16_t)y, (uint16_t)wdt, (uint16_t)hgt };
    vnc_channel_send(&w->ch, VNC_EVT_UPDATE, &r, sizeof(r));
}

static void w_on_cut_text(void *user, const char *text, size_t len)
{
    worker *w = user;
    vnc_channel_send(&w->ch, VNC_EVT_CUT_TEXT, text, (uint32_t)len);
}

static void w_on_cursor(void *user, int xhot, int yhot, int width, int height,
                        const uint8_t *bgra, const uint8_t *mask)
{
    worker *w = user;
    size_t npix = (size_t)width * (size_t)height;
    if (npix == 0 || npix > 256u * 256u)
        return;

    /* Fold the 1-byte mask into the alpha channel so the UI can build a single
     * 32bpp ARGB cursor. rcSource is B,G,R,X on our LE targets. */
    uint8_t *px = malloc(npix * 4);
    if (!px)
        return;
    memcpy(px, bgra, npix * 4);
    for (size_t i = 0; i < npix; i++)
        px[i * 4 + 3] = (mask && mask[i]) ? 0xFF : 0x00;

    vnc_ipc_cursor hdr = { (int16_t)xhot, (int16_t)yhot,
                           (uint16_t)width, (uint16_t)height };
    vnc_channel_send2(&w->ch, VNC_EVT_CURSOR, &hdr, sizeof(hdr),
                      px, (uint32_t)(npix * 4));
    free(px);
}

static void w_on_led(void *user, uint8_t state)
{
    worker *w = user;
    vnc_ipc_led led = { state };
    vnc_channel_send(&w->ch, VNC_EVT_LED, &led, sizeof(led));
}

/* Audio opt-in is enforced here too: the QEMU-audio extension is registered
 * process-wide, so a hostile server can send BEGIN/DATA even though we never sent
 * ENABLE. Drop it unless the user asked for audio, so unsolicited audio never
 * reaches the UI at all (the UI also gates playback on its own opt-in). */
static void w_on_audio_begin(void *user)
{
    (void)user; /* format was already announced at enable; begin needs no action */
}
static void w_on_audio_data(void *user, const uint8_t *pcm, size_t len)
{
    worker *w = user;
    if (!w->want_audio)
        return;
    vnc_channel_send(&w->ch, VNC_EVT_AUDIO_DATA, pcm, (uint32_t)len);
}
static void w_on_audio_end(void *user)
{
    worker *w = user;
    if (!w->want_audio)
        return;
    vnc_channel_send(&w->ch, VNC_EVT_AUDIO_END, NULL, 0);
}

static void w_on_log(void *user, vnc_log_level level, const char *msg)
{
    worker *w = user;
    uint8_t lvl = (uint8_t)level;
    /* libvncclient's log is process-global and can fire from the main thread
     * OUTSIDE api_lock (e.g. WaitForMessage's select() failing) at the same time
     * the reader thread logs from a failing input send UNDER api_lock. Those two
     * channel writes are not otherwise serialised, and vnc_channel_send2 emits
     * header+payload as separate writes, so without this lock their bytes could
     * interleave and corrupt the framed stream. Every other channel write is
     * already serialised by api_lock; log writes are the one exception. */
    mtx_lock(&w->log_lock);
    vnc_channel_send2(&w->ch, VNC_EVT_LOG, &lvl, 1,
                      msg, (uint32_t)strlen(msg));
    mtx_unlock(&w->log_lock);
}

/* Emit a worker milestone over the same log channel (for the diagnostic log). */
static void w_diag(worker *w, vnc_log_level level, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    w_on_log(w, level, buf);
}

/* Synchronous password round-trip. Runs on the main thread during connect,
 * before the reader thread exists, so it may read the channel directly. */
static char *w_get_password(void *user)
{
    worker *w = user;
    if (!vnc_channel_send(&w->ch, VNC_EVT_PASSWORD_REQ, NULL, 0))
        return NULL;
    uint32_t type = 0, len = 0;
    char buf[512];
    if (vnc_channel_recv(&w->ch, &type, buf, sizeof(buf) - 1, &len) != 1)
        return NULL;
    if (type != VNC_CMD_PASSWORD) {
        secure_wipe(buf, sizeof(buf));
        return NULL;
    }
    char *pw = malloc(len + 1);
    if (pw) {
        memcpy(pw, buf, len);
        pw[len] = '\0';
    }
    secure_wipe(buf, sizeof(buf)); /* don't leave the password on the stack */
    return pw; /* libvncclient frees it */
}

/* ---- command dispatch: IPC -> core ------------------------------------- */

static void dispatch_command(worker *w, uint32_t type, const void *buf, uint32_t len)
{
    switch (type) {
    case VNC_CMD_KEY:
        if (len == sizeof(vnc_ipc_key)) {
            const vnc_ipc_key *k = buf;
            mtx_lock(&w->api_lock);
            vnc_client_send_key(w->client, k->keysym, k->down != 0);
            mtx_unlock(&w->api_lock);
        }
        break;
    case VNC_CMD_KEY_EXT:
        if (len == sizeof(vnc_ipc_key_ext)) {
            const vnc_ipc_key_ext *k = buf;
            mtx_lock(&w->api_lock);
            vnc_client_send_key_ext(w->client, k->keysym, k->keycode, k->down != 0);
            mtx_unlock(&w->api_lock);
        }
        break;
    case VNC_CMD_POINTER:
        if (len == sizeof(vnc_ipc_pointer)) {
            const vnc_ipc_pointer *p = buf;
            mtx_lock(&w->api_lock);
            vnc_client_send_pointer(w->client, p->x, p->y, p->button_mask);
            mtx_unlock(&w->api_lock);
        }
        break;
    case VNC_CMD_CUT_TEXT:
        mtx_lock(&w->api_lock);
        vnc_client_send_cut_text(w->client, buf, len);
        mtx_unlock(&w->api_lock);
        break;
    case VNC_CMD_REQUEST_UPDATE:
        if (len == sizeof(vnc_ipc_update_req)) {
            const vnc_ipc_update_req *u = buf;
            mtx_lock(&w->api_lock);
            vnc_client_request_update(w->client, u->incremental != 0);
            mtx_unlock(&w->api_lock);
        }
        break;
    case VNC_CMD_AUDIO_ENABLE:
        if (len == sizeof(vnc_ipc_audio_cfg)) {
            const vnc_ipc_audio_cfg *a = buf;
            /* Re-validate format bounds from the (trusted-but-checked) UI. */
            if (a->channels >= 1 && a->channels <= QA_MAX_CHANNELS &&
                a->frequency >= QA_MIN_FREQ && a->frequency <= QA_MAX_FREQ) {
                mtx_lock(&w->api_lock);
                vnc_client_audio_enable(w->client, a->sample_format,
                                        a->channels, a->frequency);
                mtx_unlock(&w->api_lock);
            }
        }
        break;
    case VNC_CMD_AUDIO_DISABLE:
        mtx_lock(&w->api_lock);
        vnc_client_audio_disable(w->client);
        mtx_unlock(&w->api_lock);
        break;
    case VNC_CMD_SHUTDOWN:
        w->running = 0;
        break;
    default:
        /* Unknown/again-invalid command from a compromised UI: ignore. */
        break;
    }
}

#ifdef _WIN32
static DWORD WINAPI reader_thread(LPVOID arg)
#else
static void *reader_thread(void *arg)
#endif
{
    worker *w = arg;
    /* static: 1 MiB is too large for the default 1 MiB thread stack, and there
     * is exactly one reader thread per worker process. */
    static uint8_t buf[VNC_IPC_MAX_PAYLOAD];
    for (;;) {
        uint32_t type = 0, len = 0;
        int r = vnc_channel_recv(&w->ch, &type, buf, sizeof(buf), &len);
        if (r <= 0) {         /* EOF or malformed => tear down */
            w->running = 0;
            break;
        }
        dispatch_command(w, type, buf, len);
        if (!w->running)
            break;
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- main loop --------------------------------------------------------- */

static int worker_run(worker *w, const char *host, int port,
                      const char *encodings, bool view_only, const char *ca_file)
{
    vnc_client_delegate d = {
        .user = w,
        .on_framebuffer_update = w_on_update,
        .on_desktop_resize = w_on_resize,
        .on_cut_text = w_on_cut_text,
        .on_cursor = w_on_cursor,
        .on_led = w_on_led,
        .on_audio_begin = w_on_audio_begin,
        .on_audio_data = w_on_audio_data,
        .on_audio_end = w_on_audio_end,
        .on_log = w_on_log,
        .get_password = w_get_password,
    };
    w->client = vnc_client_create(&d);
    if (!w->client)
        return 1;
    /* Route libvncclient's global log (protocol + TLS timeline) to the UI's
     * diagnostic channel; w_on_log matches the sink signature. */
    vnc_client_set_global_log(w_on_log, w);
    if (encodings && encodings[0])
        vnc_client_set_encodings(w->client, encodings);
    if (ca_file && ca_file[0])
        vnc_client_set_ca_file(w->client, ca_file);
    vnc_client_set_view_only(w->client, view_only);
    /* Bound the core's framebuffer allocation to what our shared framebuffer can
     * hold, so a server cannot announce an oversized desktop and drive an
     * out-of-bounds copy in w_on_update. */
    vnc_client_set_max_framebuffer_bytes(w->client, w->shm_capacity);

    vnc_ipc_hello hello = { VNC_IPC_MAGIC, VNC_IPC_VERSION };
    vnc_channel_send(&w->ch, VNC_EVT_HELLO, &hello, sizeof(hello));

    w_diag(w, VNC_LOG_INFO, "worker: connecting (encodings=%s, ca=%s, view_only=%d)",
           encodings && encodings[0] ? encodings : "default",
           ca_file && ca_file[0] ? "yes" : "no", view_only);
    if (!vnc_client_connect(w->client, host, port)) {
        vnc_ipc_status st = { VNC_STATUS_CONNECT_FAILED };
        vnc_channel_send(&w->ch, VNC_EVT_STATUS, &st, sizeof(st));
        return 1;
    }
    vnc_ipc_status st = { VNC_STATUS_CONNECTED };
    vnc_channel_send(&w->ch, VNC_EVT_STATUS, &st, sizeof(st));

    /* Prime the framebuffer, then drive incremental updates ourselves. */
    mtx_lock(&w->api_lock);
    vnc_client_request_update(w->client, false);
    /* If audio was opted in, announce the format to the UI and ask the server
     * to start streaming. Done before the reader thread starts, so the send is
     * not racing another writer. */
    if (w->want_audio) {
        vnc_ipc_audio_cfg cfg = { WORKER_AUDIO_FORMAT, WORKER_AUDIO_CHANNELS,
                                  WORKER_AUDIO_FREQ };
        vnc_channel_send(&w->ch, VNC_EVT_AUDIO_FORMAT, &cfg, sizeof(cfg));
        vnc_client_audio_enable(w->client, WORKER_AUDIO_FORMAT,
                                WORKER_AUDIO_CHANNELS, WORKER_AUDIO_FREQ);
        w_diag(w, VNC_LOG_INFO, "worker: audio requested (S16 %uch %uHz)",
               WORKER_AUDIO_CHANNELS, WORKER_AUDIO_FREQ);
    }
    mtx_unlock(&w->api_lock);

    w->running = 1;
    worker_thread rt;
#ifdef _WIN32
    rt = CreateThread(NULL, 0, reader_thread, w, 0, NULL);
#else
    pthread_create(&rt, NULL, reader_thread, w);
#endif

    while (w->running) {
        /* Wait unlocked (so input sends aren't blocked during the idle wait),
         * then process the message UNDER api_lock so a server read (which may
         * decrypt TLS) never races an input write (which encrypts TLS) on the
         * same session. This serialises ALL libvncclient socket/TLS access. */
        int n = vnc_client_wait(w->client, 100000); /* 100 ms */
        if (n < 0)
            break;
        if (n > 0) {
            mtx_lock(&w->api_lock);
            int r = vnc_client_handle_message(w->client);
            if (r == 0)
                vnc_client_request_update(w->client, true);
            mtx_unlock(&w->api_lock);
            if (r != 0)
                break;
        }
    }
    w->running = 0;

    /* The reader thread is still running here (it exits only once the UI closes
     * the command channel, which the UI does in response to this very status).
     * Every other post-startup channel write happens under api_lock; take it for
     * this one too so it cannot interleave with a log the reader emits from
     * inside a libvncclient call, which would corrupt the framed byte stream.
     * (Sending it after the join would deadlock: the UI won't close the channel
     * until it sees this status.) */
    mtx_lock(&w->api_lock);
    vnc_ipc_status down = { VNC_STATUS_DISCONNECTED };
    vnc_channel_send(&w->ch, VNC_EVT_STATUS, &down, sizeof(down));
    mtx_unlock(&w->api_lock);

#ifdef _WIN32
    /* Wait indefinitely (as pthread_join does on POSIX): the reader dispatches
     * into w->client / w->shm, which we free right after returning, so it MUST be
     * dead first. It exits as soon as the UI closes the command channel (which it
     * does on seeing the status above, or when it or its process goes away), so a
     * bounded wait risked freeing the client out from under a still-live reader. */
    WaitForSingleObject(rt, INFINITE);
    CloseHandle(rt);
#else
    pthread_join(rt, NULL);
#endif
    return 0;
}

/* ---- entry point ------------------------------------------------------- */

static const char *arg_val(int argc, char **argv, const char *key)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], key))
            return argv[i + 1];
    return NULL;
}
static bool arg_flag(int argc, char **argv, const char *key)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], key))
            return true;
    return false;
}

/*
 * Worker entry point. On Windows this is invoked in-process by wWinMain when the
 * merged vncviewer.exe is re-launched with --worker into the AppContainer (the
 * product ships as ONE executable; see app/modes.h). On Linux it is reached
 * through the thin main() shim below, which the ipc_test harness spawns as the
 * standalone `vncworker` binary.
 */
int vnc_worker_main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    /* A dead UI peer must make our next channel write FAIL (so we tear down
     * cleanly), not kill us by signal. Windows WriteFile already returns an
     * error for a broken pipe; POSIX needs SIGPIPE ignored. */
    signal(SIGPIPE, SIG_IGN);
#endif
    const char *shm_name = arg_val(argc, argv, "--shm");         /* POSIX test */
    const char *shm_handle_s = arg_val(argc, argv, "--shm-handle"); /* Windows */
    const char *shm_bytes_s = arg_val(argc, argv, "--shm-bytes");
    const char *host = arg_val(argc, argv, "--host");
    const char *port_s = arg_val(argc, argv, "--port");
    const char *rd_s = arg_val(argc, argv, "--rd");
    const char *wr_s = arg_val(argc, argv, "--wr");
    const char *encodings = arg_val(argc, argv, "--encodings");
    const char *ca_file = arg_val(argc, argv, "--ca");
    bool view_only = arg_flag(argc, argv, "--view-only");
    bool want_audio = arg_flag(argc, argv, "--audio");

    if ((!shm_name && !shm_handle_s) || !shm_bytes_s || !host || !port_s ||
        !rd_s || !wr_s) {
        fprintf(stderr, "vncworker: missing required arguments\n");
        return 2;
    }

    worker w;
    memset(&w, 0, sizeof(w));
    mtx_init(&w.api_lock);
    mtx_init(&w.log_lock);
    w.want_audio = want_audio;

    size_t shm_bytes = (size_t)strtoull(shm_bytes_s, NULL, 10);
#ifdef _WIN32
    /* An AppContainer cannot open the mapping by name (separate object
     * namespace), so the UI hands it in as an inherited handle. */
    if (shm_handle_s)
        w.shm = vnc_shm_from_handle((void *)(uintptr_t)_strtoui64(shm_handle_s, NULL, 10),
                                    shm_bytes);
    else
        w.shm = vnc_shm_open(shm_name, shm_bytes);
#else
    w.shm = vnc_shm_open(shm_name, shm_bytes);
#endif
    if (!w.shm) {
        fprintf(stderr, "vncworker: cannot open shared framebuffer\n");
        return 1;
    }
    w.shm_capacity = vnc_shm_capacity(w.shm);

#ifdef _WIN32
    w.ch.rd = (vnc_handle)(intptr_t)_atoi64(rd_s);
    w.ch.wr = (vnc_handle)(intptr_t)_atoi64(wr_s);
#else
    w.ch.rd = (vnc_handle)strtol(rd_s, NULL, 10);
    w.ch.wr = (vnc_handle)strtol(wr_s, NULL, 10);
#endif

    int rc = worker_run(&w, host, (int)strtol(port_s, NULL, 10),
                        encodings, view_only, ca_file);

    /* Detach the process-global libvncclient log from &w before we tear it down:
     * rfbClientCleanup() (inside destroy) can log, and must not fire w_on_log with
     * a worker whose channel/locks are going away. */
    vnc_client_set_global_log(NULL, NULL);
    if (w.client)
        vnc_client_destroy(w.client);
    vnc_shm_close(w.shm);
    return rc;
}

#ifdef VNC_WORKER_STANDALONE
/* Standalone `vncworker` executable (Linux/CI: spawned by ipc_test). On Windows
 * the worker is a mode of the single vncviewer.exe and this shim is compiled out. */
int main(int argc, char **argv)
{
    return vnc_worker_main(argc, argv);
}
#endif
