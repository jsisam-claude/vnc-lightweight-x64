/*
 * headless_main.c — portable console VNC test client (vnctest).
 *
 * Not the product. A verification tool: connects, negotiates a chosen encoding,
 * pumps updates, and emits a stable FNV-1a checksum of the framebuffer so the
 * per-encoding checksum matrix (see docs/TESTING.md) can assert every decoder
 * reproduces identical pixels. Optionally writes a PPM snapshot.
 *
 * Runs on Linux (primary CI, under ASan/UBSan) and Windows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#endif

#include "app/modes.h"
#include "core/client.h"
#include "core/png_write.h"

struct app {
    int fb_width, fb_height;
    long updates;
};

static void on_update(void *user, int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
    ((struct app *)user)->updates++;
}

static void on_resize(void *user, int width, int height)
{
    struct app *a = user;
    a->fb_width = width;
    a->fb_height = height;
}

static void on_cut_text(void *user, const char *text, size_t len)
{
    (void)user; (void)text;
    fprintf(stderr, "[vnctest] server cut text: %zu bytes\n", len);
}

static const char *level_name(vnc_log_level l)
{
    switch (l) {
    case VNC_LOG_ERROR: return "ERROR";
    case VNC_LOG_WARN:  return "WARN";
    case VNC_LOG_INFO:  return "INFO";
    default:            return "DEBUG";
    }
}

static void on_log(void *user, vnc_log_level level, const char *msg)
{
    (void)user;
    fprintf(stderr, "[vnctest] %s: %s\n", level_name(level), msg);
}

/* Password strictly from the environment (never argv), test-only convenience. */
static char *on_get_password(void *user)
{
    (void)user;
    const char *p = getenv("VNC_PASSWORD");
    if (!p)
        return NULL;
    char *dup = strdup(p);
    return dup; /* freed by libvncclient */
}

/* Checksum the VISIBLE pixels only: 3 of every 4 bytes. The 4th byte is unused
 * padding in our 32bpp framebuffer (GDI ignores it when blitting a BI_RGB DIB),
 * and different decoders legitimately leave it at different values — hashing it
 * would make the per-encoding matrix compare a don't-care byte. */
static uint64_t fnv1a_rgb(const uint8_t *fb, int w, int h)
{
    uint64_t hash = 1469598103934665603ULL;
    size_t npix = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npix; i++) {
        for (int b = 0; b < 3; b++) {
            hash ^= fb[i * 4 + b];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

static int write_ppm(const char *path, const uint8_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    /* Framebuffer is 32bpp; rfbGetClient(8,3,4) yields RGBX byte order on LE. */
    for (int i = 0; i < w * h; i++) {
        fputc(fb[i * 4 + 0], f);
        fputc(fb[i * 4 + 1], f);
        fputc(fb[i * 4 + 2], f);
    }
    fclose(f);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s HOST[:PORT] [--encodings \"...\"] [--frames N]\n"
        "          [--timeout-ms MS] [--ppm FILE] [--png FILE] [--resize WxH]\n"
        "          [--ca FILE] [--view-only]\n"
        "  Password (if required) is read from $VNC_PASSWORD.\n",
        argv0);
}

/*
 * Headless test-client entry point. On Windows this is invoked in-process by
 * wWinMain when the merged vncviewer.exe is launched with --headless (a console
 * diagnostic mode). On Linux it is reached through the thin main() shim below,
 * built as the standalone `vnctest` binary used by CI.
 */
int vnc_headless_main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    const char *target = argv[1];
    const char *encodings = NULL;
    const char *ppm_path = NULL;
    const char *png_path = NULL;
    const char *ca_file = NULL;
    long want_frames = 1;
    int timeout_ms = 5000;
    bool view_only = false;
    int resize_w = 0, resize_h = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--encodings") && i + 1 < argc)
            encodings = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            want_frames = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc)
            timeout_ms = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
            ppm_path = argv[++i];
        else if (!strcmp(argv[i], "--png") && i + 1 < argc)
            png_path = argv[++i];
        else if (!strcmp(argv[i], "--resize") && i + 1 < argc) {
            /* WxH: ask the server to resize (ExtendedDesktopSize). */
            const char *s = argv[++i];
            resize_w = (int)strtol(s, NULL, 10);
            const char *x = strchr(s, 'x');
            resize_h = x ? (int)strtol(x + 1, NULL, 10) : 0;
        }
        else if (!strcmp(argv[i], "--ca") && i + 1 < argc)
            ca_file = argv[++i];
        else if (!strcmp(argv[i], "--view-only"))
            view_only = true;
        else {
            fprintf(stderr, "[vnctest] unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    /* Split HOST[:PORT]; default RFB port 5900. */
    char host[256];
    int port = 5900;
    snprintf(host, sizeof(host), "%s", target); /* portable + safe (no strncpy) */
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = (int)strtol(colon + 1, NULL, 10);
    }

    struct app app = {0};
    vnc_client_delegate d = {
        .user = &app,
        .on_framebuffer_update = on_update,
        .on_desktop_resize = on_resize,
        .on_cut_text = on_cut_text,
        .on_log = on_log,
        .get_password = on_get_password,
    };

    vnc_client *c = vnc_client_create(&d);
    if (!c) {
        fprintf(stderr, "[vnctest] out of memory\n");
        return 1;
    }
    if (encodings)
        vnc_client_set_encodings(c, encodings);
    if (ca_file)
        vnc_client_set_ca_file(c, ca_file);
    vnc_client_set_view_only(c, view_only);

    if (!vnc_client_connect(c, host, port)) {
        fprintf(stderr, "[vnctest] connect to %s:%d failed\n", host, port);
        vnc_client_destroy(c);
        return 1;
    }

    fprintf(stderr, "[vnctest] connected: %s (%dx%d), encodings=%s\n",
            vnc_client_desktop_name(c) ? vnc_client_desktop_name(c) : "(none)",
            vnc_client_width(c), vnc_client_height(c),
            encodings ? encodings : "(default)");

    vnc_client_request_update(c, false);

    int elapsed = 0;
    const int step_ms = 100;
    while (app.updates < want_frames && elapsed < timeout_ms) {
        int r = vnc_client_pump(c, (unsigned)step_ms * 1000u);
        if (r < 0) {
            fprintf(stderr, "[vnctest] disconnected during pump\n");
            vnc_client_destroy(c);
            return 1;
        }
        if (r == 0)
            elapsed += step_ms;
        vnc_client_request_update(c, true);
    }

    /* Client-driven resize: only meaningful AFTER a frame, when the server's
     * screen geometry (ExtendedDesktopSize) has been received. Request it, then
     * pump until the desktop actually changes size (or we time out). */
    if (resize_w > 0 && resize_h > 0) {
        fprintf(stderr, "[vnctest] requesting resize to %dx%d\n", resize_w, resize_h);
        int e2 = 0;
        while ((vnc_client_width(c) != resize_w || vnc_client_height(c) != resize_h) &&
               e2 < timeout_ms) {
            vnc_client_request_desktop_size(c, resize_w, resize_h);
            int r = vnc_client_pump(c, (unsigned)step_ms * 1000u);
            if (r < 0)
                break;
            if (r == 0)
                e2 += step_ms;
            vnc_client_request_update(c, true);
        }
        fprintf(stderr, "[vnctest] after resize: %dx%d\n",
                vnc_client_width(c), vnc_client_height(c));
    }

    const uint8_t *fb = vnc_client_framebuffer(c);
    int w = vnc_client_width(c), h = vnc_client_height(c);
    if (!fb || w <= 0 || h <= 0) {
        fprintf(stderr, "[vnctest] no framebuffer received\n");
        vnc_client_destroy(c);
        return 1;
    }

    uint64_t sum = fnv1a_rgb(fb, w, h);
    printf("checksum=%016llx width=%d height=%d updates=%ld\n",
           (unsigned long long)sum, w, h, app.updates);

    if (ppm_path && write_ppm(ppm_path, fb, w, h) != 0)
        fprintf(stderr, "[vnctest] failed to write PPM %s\n", ppm_path);
    if (png_path && !png_write_bgrx(png_path, fb, w, h))
        fprintf(stderr, "[vnctest] failed to write PNG %s\n", png_path);

    vnc_client_destroy(c);
    return 0;
}

#ifdef VNC_HEADLESS_STANDALONE
/* Standalone `vnctest` executable (Linux/CI). On Windows the headless client is
 * the --headless mode of the single vncviewer.exe and this shim is compiled out. */
int main(int argc, char **argv)
{
    return vnc_headless_main(argc, argv);
}
#endif
