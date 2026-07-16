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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/client.h"
#include "ipc/channel.h"
#include "ipc/protocol.h"
#include "ipc/shm.h"

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
    volatile int  running;
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
    for (int row = 0; row < hgt; row++) {
        size_t off = ((size_t)(y + row) * (size_t)fbw + (size_t)x) * 4u;
        memcpy(dst + off, fb + off, (size_t)wdt * 4u);
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

static void w_on_log(void *user, vnc_log_level level, const char *msg)
{
    worker *w = user;
    uint8_t lvl = (uint8_t)level;
    vnc_channel_send2(&w->ch, VNC_EVT_LOG, &lvl, 1,
                      msg, (uint32_t)strlen(msg));
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
    if (type != VNC_CMD_PASSWORD)
        return NULL;
    char *pw = malloc(len + 1);
    if (!pw)
        return NULL;
    memcpy(pw, buf, len);
    pw[len] = '\0';
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
    for (;;) {
        uint32_t type = 0, len = 0;
        uint8_t buf[VNC_IPC_MAX_PAYLOAD];
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
                      const char *encodings, bool view_only)
{
    vnc_client_delegate d = {
        .user = w,
        .on_framebuffer_update = w_on_update,
        .on_desktop_resize = w_on_resize,
        .on_cut_text = w_on_cut_text,
        .on_cursor = w_on_cursor,
        .on_log = w_on_log,
        .get_password = w_get_password,
    };
    w->client = vnc_client_create(&d);
    if (!w->client)
        return 1;
    if (encodings && encodings[0])
        vnc_client_set_encodings(w->client, encodings);
    vnc_client_set_view_only(w->client, view_only);

    vnc_ipc_hello hello = { VNC_IPC_MAGIC, VNC_IPC_VERSION };
    vnc_channel_send(&w->ch, VNC_EVT_HELLO, &hello, sizeof(hello));

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
    mtx_unlock(&w->api_lock);

    w->running = 1;
    worker_thread rt;
#ifdef _WIN32
    rt = CreateThread(NULL, 0, reader_thread, w, 0, NULL);
#else
    pthread_create(&rt, NULL, reader_thread, w);
#endif

    while (w->running) {
        int n = vnc_client_pump(w->client, 100000); /* 100 ms, unlocked wait */
        if (n < 0)
            break;
        if (n > 0) {
            mtx_lock(&w->api_lock);
            vnc_client_request_update(w->client, true);
            mtx_unlock(&w->api_lock);
        }
    }
    w->running = 0;

    vnc_ipc_status down = { VNC_STATUS_DISCONNECTED };
    vnc_channel_send(&w->ch, VNC_EVT_STATUS, &down, sizeof(down));

#ifdef _WIN32
    WaitForSingleObject(rt, 1000);
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

int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const char *shm_name = arg_val(argc, argv, "--shm");         /* POSIX test */
    const char *shm_handle_s = arg_val(argc, argv, "--shm-handle"); /* Windows */
    const char *shm_bytes_s = arg_val(argc, argv, "--shm-bytes");
    const char *host = arg_val(argc, argv, "--host");
    const char *port_s = arg_val(argc, argv, "--port");
    const char *rd_s = arg_val(argc, argv, "--rd");
    const char *wr_s = arg_val(argc, argv, "--wr");
    const char *encodings = arg_val(argc, argv, "--encodings");
    bool view_only = arg_flag(argc, argv, "--view-only");

    if ((!shm_name && !shm_handle_s) || !shm_bytes_s || !host || !port_s ||
        !rd_s || !wr_s) {
        fprintf(stderr, "vncworker: missing required arguments\n");
        return 2;
    }

    worker w;
    memset(&w, 0, sizeof(w));
    mtx_init(&w.api_lock);

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
                        encodings, view_only);

    if (w.client)
        vnc_client_destroy(w.client);
    vnc_shm_close(w.shm);
    return rc;
}
