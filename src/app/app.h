/*
 * Trusted UI/broker process (vncviewer) — shared declarations.
 *
 * This process owns the window, GDI blitting, input capture, clipboard, and
 * (later) audio and TLS. It spawns the untrusted vncworker into an AppContainer
 * sandbox and talks to it only through the validated IPC channel + shared
 * framebuffer. It contains NO RFB-parsing code.
 */
#ifndef VNC_APP_H
#define VNC_APP_H

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
/* winsock2.h must precede windows.h; WIN32_LEAN_AND_MEAN otherwise keeps
 * windows.h from pulling in the (conflicting) winsock.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "ipc/channel.h"
#include "ipc/shm.h"

/* Custom messages posted from the reader thread to the UI thread. */
#define WM_APP_RESIZE   (WM_APP + 1) /* wParam=width, lParam=height */
#define WM_APP_UPDATE   (WM_APP + 2) /* lParam -> heap vnc_ipc_rect (UI frees) */
#define WM_APP_CUTTEXT  (WM_APP + 3) /* lParam -> heap {u32 len; bytes} (UI frees) */
#define WM_APP_STATUS   (WM_APP + 4) /* wParam=status code */
#define WM_APP_PWREQ    (WM_APP + 5) /* worker needs a password */
#define WM_APP_CURSOR   (WM_APP + 6) /* lParam -> heap cursor blob (UI frees) */
#define WM_APP_LED      (WM_APP + 7) /* wParam = keyboard LED state bitmask */

typedef struct {
    /* IPC to worker */
    vnc_channel ch;
    vnc_shm    *shm;
    size_t      shm_bytes;
    char        shm_name[64];
    HANDLE      worker_process;
    HANDLE      reader_thread;

    /* Window + presentation */
    HWND        hwnd;
    int         fb_width, fb_height;   /* current framebuffer dimensions */
    BITMAPINFO  bmi;                   /* describes the shm pixels as a DIB */
    HCURSOR     remote_cursor;         /* current server-supplied cursor */
    BOOL        ignore_clip_update;    /* suppress echo of server-set clipboard */

    /* Connection parameters */
    wchar_t     host[256];
    int         port;
    BOOL        view_only;
    BOOL        connected;
} ViewerApp;

/* viewer_window.c */
ATOM  viewer_register_class(HINSTANCE hinst);
HWND  viewer_create_window(ViewerApp *app, HINSTANCE hinst);
DWORD WINAPI viewer_reader_thread(LPVOID arg); /* pumps worker events */

/* input_win32.c — translate + forward input to the worker. */
void  input_key(ViewerApp *app, WPARAM vk, LPARAM lparam, BOOL down);
void  input_pointer(ViewerApp *app, int x, int y, UINT msg, WPARAM wparam);

/* clipboard_win32.c — both directions. */
void  clipboard_from_server(ViewerApp *app, const char *text, unsigned len);
void  clipboard_to_server(ViewerApp *app); /* read local clipboard -> worker */

/* viewer_window.c — build + apply a server-supplied cursor from an EVT_CURSOR
 * blob (vnc_ipc_cursor header followed by BGRA pixels). */
void  viewer_set_cursor(ViewerApp *app, const uint8_t *blob, unsigned len);

/* main_win32.c — prompt for a password and send it to the worker. */
void  app_request_password(ViewerApp *app);

/* sandbox_win32.c — spawn the worker inside an AppContainer. */
typedef struct {
    const char *host;      /* UTF-8 */
    int         port;
    const char *encodings; /* may be NULL */
    BOOL        view_only;
    const char *shm_name;
    size_t      shm_bytes;
} WorkerSpawnParams;

/* On success fills app->ch, app->worker_process. Returns FALSE on failure.
 * Fails closed: if the sandbox cannot be created, the worker is NOT spawned
 * unsandboxed (unless VNC_ALLOW_UNSANDBOXED is defined at build time). */
BOOL  sandbox_spawn_worker(ViewerApp *app, const WorkerSpawnParams *p);
void  sandbox_cleanup(ViewerApp *app);

#endif /* VNC_APP_H */
