/*
 * ipc_test — Linux-only cross-process verification of the M2 pipeline.
 *
 * Plays the UI/broker role WITHOUT any GUI: creates the shared framebuffer and
 * a socketpair, forks+execs the real vncworker, performs the startup handshake,
 * pumps events, then checksums the shared framebuffer's visible pixels. The
 * checksum must match vnctest's for the same server+encoding — i.e. the sandbox
 * worker + IPC + shared memory reproduce identical pixels to the direct client.
 *
 * This is the faithful in-container test for code whose Windows GUI/sandbox
 * cannot run here; only the AppContainer wrapping differs on Windows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc/channel.h"
#include "ipc/protocol.h"
#include "ipc/shm.h"

/* Same visible-pixel checksum as vnctest (RGB of every 32bpp pixel). */
static uint64_t fnv1a_rgb(const uint8_t *fb, uint32_t w, uint32_t h)
{
    uint64_t hash = 1469598103934665603ULL;
    size_t npix = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npix; i++)
        for (int b = 0; b < 3; b++) {
            hash ^= fb[i * 4 + b];
            hash *= 1099511628211ULL;
        }
    return hash;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s HOST:PORT [--encodings E] [--worker PATH]\n"
                        "  password from $VNC_PASSWORD; needs vncworker on PATH\n",
                argv[0]);
        return 2;
    }
    const char *target = argv[1];
    const char *encodings = NULL;
    const char *worker_path = "./vncworker";
    long want_updates = 3;
    int want_audio = 0;
    int verbose = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--encodings") && i + 1 < argc) encodings = argv[++i];
        else if (!strcmp(argv[i], "--worker") && i + 1 < argc) worker_path = argv[++i];
        else if (!strcmp(argv[i], "--updates") && i + 1 < argc) want_updates = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--audio")) want_audio = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    }

    char host[256];
    int port = 5900;
    strncpy(host, target, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    char *colon = strrchr(host, ':');
    if (colon) { *colon = '\0'; port = (int)strtol(colon + 1, NULL, 10); }

    /* Shared framebuffer sized for the maximum desktop we accept (16384^2 * 4
     * would be 1 GiB; use a realistic 8 MiB cap = ~1448x1448, plenty for test). */
    const size_t max_bytes = 8u * 1024 * 1024;
    char shm_name[64];
    vnc_shm *shm = vnc_shm_create(max_bytes, shm_name, sizeof(shm_name));
    if (!shm) { perror("shm_create"); return 1; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* child: worker. Keep sv[1]; it must survive exec. */
        close(sv[0]);
        char rd[16], wr[16], bytes[32], port_s[16];
        snprintf(rd, sizeof(rd), "%d", sv[1]);
        snprintf(wr, sizeof(wr), "%d", sv[1]);
        snprintf(bytes, sizeof(bytes), "%zu", max_bytes);
        snprintf(port_s, sizeof(port_s), "%d", port);
        char *args[20];
        int n = 0;
        args[n++] = (char *)worker_path;
        args[n++] = "--shm";   args[n++] = shm_name;
        args[n++] = "--shm-bytes"; args[n++] = bytes;
        args[n++] = "--host";  args[n++] = host;
        args[n++] = "--port";  args[n++] = port_s;
        args[n++] = "--rd";    args[n++] = rd;
        args[n++] = "--wr";    args[n++] = wr;
        if (encodings) { args[n++] = "--encodings"; args[n++] = (char *)encodings; }
        if (want_audio) { args[n++] = "--audio"; }
        args[n] = NULL;
        execv(worker_path, args);
        perror("execv vncworker");
        _exit(127);
    }

    /* parent: UI role. */
    close(sv[1]);
    vnc_channel ch = { (vnc_handle)sv[0], (vnc_handle)sv[0] };

    uint32_t width = 0, height = 0;
    long updates = 0;
    int connected = 0, rc = 1;
    int audio_format_seen = 0;
    uint64_t audio_bytes = 0;
    uint8_t buf[VNC_IPC_MAX_PAYLOAD];

    for (;;) {
        uint32_t type = 0, len = 0;
        int r = vnc_channel_recv(&ch, &type, buf, sizeof(buf), &len);
        if (r <= 0) {
            fprintf(stderr, "[ipc_test] channel closed (r=%d)\n", r);
            break;
        }
        switch (type) {
        case VNC_EVT_HELLO: {
            if (len == sizeof(vnc_ipc_hello)) {
                vnc_ipc_hello *h = (void *)buf;
                if (h->magic != VNC_IPC_MAGIC || h->version != VNC_IPC_VERSION) {
                    fprintf(stderr, "[ipc_test] bad hello\n");
                    goto done;
                }
            }
            break;
        }
        case VNC_EVT_PASSWORD_REQ: {
            const char *pw = getenv("VNC_PASSWORD");
            if (!pw) pw = "";
            vnc_channel_send(&ch, VNC_CMD_PASSWORD, pw, (uint32_t)strlen(pw));
            break;
        }
        case VNC_EVT_STATUS: {
            if (len == sizeof(vnc_ipc_status)) {
                uint8_t code = buf[0];
                if (code == VNC_STATUS_CONNECTED) connected = 1;
                else if (code == VNC_STATUS_CONNECT_FAILED ||
                         code == VNC_STATUS_AUTH_FAILED) {
                    fprintf(stderr, "[ipc_test] status=%u (failed)\n", code);
                    goto done;
                } else if (code == VNC_STATUS_DISCONNECTED && !connected) {
                    goto done;
                }
            }
            break;
        }
        case VNC_EVT_RESIZE:
            if (len == sizeof(vnc_ipc_resize)) {
                vnc_ipc_resize *rz = (void *)buf;
                width = rz->width; height = rz->height;
            }
            break;
        case VNC_EVT_UPDATE:
            updates++;
            if (updates >= want_updates && width && height) {
                uint64_t sum = fnv1a_rgb(vnc_shm_pixels(shm), width, height);
                printf("checksum=%016llx width=%u height=%u updates=%ld\n",
                       (unsigned long long)sum, width, height, updates);
                vnc_channel_send(&ch, VNC_CMD_SHUTDOWN, NULL, 0);
                rc = 0;
                goto done;
            }
            /* keep frames flowing */
            { vnc_ipc_update_req u = { 1 };
              vnc_channel_send(&ch, VNC_CMD_REQUEST_UPDATE, &u, sizeof(u)); }
            break;
        case VNC_EVT_LOG:
            if (verbose && len >= 1) {
                uint32_t mlen = len - 1;
                fprintf(stderr, "[worker:%u] %.*s\n", buf[0], (int)mlen, buf + 1);
            }
            break;
        case VNC_EVT_AUDIO_FORMAT:
            audio_format_seen = 1;
            break;
        case VNC_EVT_AUDIO_DATA:
            audio_bytes += len;
            break;
        default:
            break; /* ignore log/cut-text/etc. for this test */
        }
    }
done:
    if (want_audio)
        printf("audio_format=%d audio_bytes=%llu\n", audio_format_seen,
               (unsigned long long)audio_bytes);
    close(sv[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    vnc_shm_close(shm);
    return rc;
}
