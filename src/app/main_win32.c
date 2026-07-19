/*
 * main_win32.c — entry point for the trusted UI/broker process (vncviewer).
 *
 * Orchestrates: parse target, create the shared framebuffer, create the window,
 * spawn the sandboxed worker, run the message loop while a reader thread pumps
 * worker events. Also owns the password prompt (credentials never live in the
 * sandbox longer than the handshake, and never appear on a command line).
 */
#include "app/app.h"
#include "app/audio_waveout.h"
#include "app/diag.h"
#include "app/modes.h"

#include <commdlg.h>   /* GetOpenFileNameW (delay-loaded: UI mode only) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Framebuffer mapping is sized for up to ~4K (3840x2160x4 ≈ 33 MiB); round up. */
#define VIEWER_SHM_BYTES (64u * 1024u * 1024u)

static ViewerApp g_app;

/* ---- password prompt (a small modal popup with an ES_PASSWORD edit) ------- */

static wchar_t g_pw_buf[512];
static BOOL    g_pw_ok;

static LRESULT CALLBACK pw_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == 1)) {
            HWND edit = GetDlgItem(hwnd, 100);
            GetWindowTextW(edit, g_pw_buf, 512);
            g_pw_ok = TRUE;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == 2) {
            g_pw_ok = FALSE;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_pw_ok = FALSE;
        DestroyWindow(hwnd);
        return 0;
    /* No PostQuitMessage here: the nested prompt loop exits via its IsWindow()
     * check. Posting WM_QUIT would also tear down the main message loop. */
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL prompt_password(HWND parent, HINSTANCE hinst)
{
    static const wchar_t *cls = L"VncPwPrompt";
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = pw_proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered = TRUE;
    }

    g_pw_ok = FALSE;
    SecureZeroMemory(g_pw_buf, sizeof(g_pw_buf));

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, cls,
        L"Authentication required",
        WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 140,
        parent, NULL, hinst, NULL);
    if (!dlg)
        return FALSE;

    CreateWindowExW(0, L"STATIC", L"Password:",
        WS_CHILD | WS_VISIBLE, 12, 14, 280, 18, dlg, NULL, hinst, NULL);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
        12, 36, 280, 24, dlg, (HMENU)100, hinst, NULL);
    CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        128, 72, 75, 26, dlg, (HMENU)1, hinst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        216, 72, 75, 26, dlg, (HMENU)2, hinst, NULL);

    EnableWindow(parent, FALSE);
    SetFocus(edit);

    MSG msg;
    BOOL got;
    while ((got = GetMessageW(&msg, NULL, 0, 0)) != 0) {
        if (got == -1)
            break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(dlg, WM_COMMAND, IDOK, 0);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
            continue;
        }
        /* If the session dies while this prompt is open (worker crash mid-auth),
         * do NOT dispatch the status here: its handler tears the session down and
         * restarts it (a whole new worker/channel) re-entrantly underneath the
         * open dialog, and the password would then be sent to the wrong session.
         * Cancel the prompt and re-post the status so teardown runs in the OUTER
         * loop once this dialog is gone. */
        if (msg.hwnd == parent && msg.message == WM_APP_STATUS &&
            (int)msg.wParam != VNC_STATUS_CONNECTED) {
            g_pw_ok = FALSE;
            DestroyWindow(dlg);
            PostMessageW(parent, WM_APP_STATUS, msg.wParam, 0);
            break;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(dlg))
            break;
    }
    /* If a WM_QUIT arrived during the prompt (e.g. the worker died and the main
     * window was destroyed), re-post it so the OUTER message loop also exits. */
    if (got == 0)
        PostQuitMessage((int)msg.wParam);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return g_pw_ok;
}

void app_request_password(ViewerApp *app)
{
    HINSTANCE hinst = (HINSTANCE)GetModuleHandleW(NULL);
    /* g_pw_buf holds up to 511 wchars; UTF-8 needs up to 3 bytes/char (+NUL).
     * A 512-byte buffer silently failed (WideCharToMultiByte -> 0) for long or
     * non-ASCII passwords, sending an empty password. Size for the worst case. */
    char utf8[512 * 3 + 1];
    if (prompt_password(app->hwnd, hinst)) {
        int n = WideCharToMultiByte(CP_UTF8, 0, g_pw_buf, -1, utf8, sizeof(utf8),
                                    NULL, NULL);
        if (n > 0)
            vnc_channel_send(&app->ch, VNC_CMD_PASSWORD, utf8, (uint32_t)(n - 1));
        else
            vnc_channel_send(&app->ch, VNC_CMD_PASSWORD, "", 0);
        SecureZeroMemory(utf8, sizeof(utf8));
    } else {
        vnc_channel_send(&app->ch, VNC_CMD_PASSWORD, "", 0);
    }
    SecureZeroMemory(g_pw_buf, sizeof(g_pw_buf));
}

/* ---- session lifecycle (startup + reconnect) ----------------------------- */

BOOL app_start_session(ViewerApp *app)
{
    app->button_mask = 0; /* no buttons held at the start of a (re)connection */
    char host_utf8[256];
    WideCharToMultiByte(CP_UTF8, 0, app->host, -1, host_utf8, sizeof(host_utf8),
                        NULL, NULL);
    WorkerSpawnParams sp = {
        .host = host_utf8,
        .port = app->port,
        .encodings = NULL,
        .view_only = app->view_only,
        .audio = app->want_audio,
        .ca_file = app->ca_file[0] ? app->ca_file : NULL,
        .shm_name = app->shm_name,
        .shm_bytes = app->shm_bytes,
    };
    if (!sandbox_spawn_worker(app, &sp))
        return FALSE;
    app->reader_thread = CreateThread(NULL, 0, viewer_reader_thread, app, 0, NULL);
    return app->reader_thread != NULL;
}

void app_stop_session(ViewerApp *app)
{
    if (app->ch.wr)
        vnc_channel_send(&app->ch, VNC_CMD_SHUTDOWN, NULL, 0);
    sandbox_cleanup(app); /* closes channel, waits for / terminates worker */
    if (app->reader_thread) {
        /* Wait indefinitely: sandbox_cleanup already closed the event channel, so
         * the reader's blocking recv returns at once and it exits. A bounded wait
         * could return while the reader is still live and then app_start_session
         * would spawn a second reader that races this one over app->audio and the
         * event stream. The reader only PostMessages (never SendMessage), so it
         * cannot deadlock against this (non-pumping) thread. */
        WaitForSingleObject(app->reader_thread, INFINITE);
        CloseHandle(app->reader_thread);
        app->reader_thread = NULL;
    }
    /* Reader thread joined: drop any stale events it queued (freeing payloads)
     * so they cannot leak or disturb a reconnected session. */
    viewer_drain_messages(app->hwnd);
    if (app->audio) {
        waveout_destroy(app->audio);
        app->audio = NULL;
    }
    app->fb_width = app->fb_height = 0;
    app->connected = FALSE;
}

/* ---- single-executable mode dispatch ------------------------------------- */
/*
 * The product ships as ONE vncviewer.exe with three modes (see app/modes.h):
 *   (default)   trusted UI/broker
 *   --worker    sandboxed decoder child (the UI re-launches itself with this)
 *   --headless  console diagnostic client (the Linux `vnctest`)
 *
 * The worker mode must NOT load user32/gdi32 (win32k is disabled by mitigation).
 * The GUI DLLs are delay-loaded so they are never pulled in unless the UI calls
 * them — which also means the mode dispatch itself may not use shell32
 * (CommandLineToArgvW lives there and would drag in user32). We therefore parse
 * the command line ourselves with the standard MSVCRT argv rules. */
static wchar_t **cmdline_to_wargv(const wchar_t *cmd, int *argc_out)
{
    size_t len = wcslen(cmd);
    /* Upper bounds: at most len+2 tokens; output chars <= len+1. */
    wchar_t **argv = (wchar_t **)malloc((len + 2) * sizeof(wchar_t *));
    wchar_t  *out  = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!argv || !out) { free(argv); free(out); *argc_out = 0; return NULL; }

    int argc = 0;
    const wchar_t *p = cmd;
    wchar_t *w = out;
    while (*p) {
        while (*p == L' ' || *p == L'\t') p++;   /* skip inter-arg whitespace */
        if (!*p) break;
        argv[argc++] = w;
        int in_quotes = 0;
        for (;;) {
            unsigned nbs = 0;
            while (*p == L'\\') { nbs++; p++; }
            if (*p == L'"') {
                /* 2n backslashes + " -> n backslashes, toggle quote; 2n+1 -> n
                 * backslashes + literal ". */
                while (nbs >= 2) { *w++ = L'\\'; nbs -= 2; }
                if (nbs == 1) { *w++ = L'"'; p++; continue; }
                in_quotes = !in_quotes;
                p++;
                continue;
            }
            while (nbs--) *w++ = L'\\';
            if (!*p) break;
            if (!in_quotes && (*p == L' ' || *p == L'\t')) break;
            *w++ = *p++;
        }
        *w++ = 0;
    }
    argv[argc] = NULL;
    *argc_out = argc;
    return argv; /* single free(argv) + free(argv[0]) frees everything */
}

static void free_wargv(wchar_t **argv)
{
    if (argv) { free(argv[0]); free(argv); }
}

/* Convert a wide argv to a freshly-allocated UTF-8 char** (NULL-terminated) for
 * the portable worker/headless entry points. */
static char **wargv_to_utf8(wchar_t **wargv, int argc)
{
    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!argv) return NULL;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char *)malloc(n > 0 ? (size_t)n : 1);
        if (argv[i] && n > 0)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
        else if (argv[i])
            argv[i][0] = '\0';
    }
    return argv;
}

static void free_utf8_argv(char **argv, int argc)
{
    if (!argv) return;
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
}

/* Is `flag` present anywhere on the wide command line? Used for mode detection
 * before any shell32/user32 code runs. */
static BOOL wargv_has(wchar_t **wargv, int argc, const wchar_t *flag)
{
    for (int i = 1; i < argc; i++)
        if (!wcscmp(wargv[i], flag)) return TRUE;
    return FALSE;
}

/* Run a portable (char** argv) entry point with a UTF-8-converted argv. */
static int run_utf8_mode(int (*entry)(int, char **), wchar_t **wargv, int argc)
{
    char **u8 = wargv_to_utf8(wargv, argc);
    if (!u8) return 1;
    int rc = entry(argc, u8);
    free_utf8_argv(u8, argc);
    return rc;
}

/* ---- connection dialog (shown when launched with no target) --------------- */

static ViewerApp *g_conn_app;
static BOOL        g_conn_ok;

static LRESULT CALLBACK conn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == 206) { /* Browse for a CA file */
            wchar_t file[1024] = L"";
            OPENFILENAMEW ofn = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"PEM certificates (*.pem;*.crt)\0*.pem;*.crt\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = 1024;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn))
                SetWindowTextW(GetDlgItem(hwnd, 205), file);
            return 0;
        }
        if (LOWORD(wp) == 1) { /* Connect */
            ViewerApp *a = g_conn_app;
            wchar_t hostbuf[256], portbuf[16];
            GetWindowTextW(GetDlgItem(hwnd, 201), hostbuf, 256);
            GetWindowTextW(GetDlgItem(hwnd, 202), portbuf, 16);
            /* trim leading AND trailing spaces on host; reject empty */
            wchar_t *hs = hostbuf;
            while (*hs == L' ') hs++;
            size_t hl = wcslen(hs);
            while (hl > 0 && hs[hl - 1] == L' ') hs[--hl] = 0;
            if (!*hs) { MessageBoxW(hwnd, L"Please enter a host.",
                                    L"VNC Lightweight", MB_OK | MB_ICONWARNING); return 0; }
            wcsncpy_s(a->host, 256, hs, _TRUNCATE);
            a->port = (int)wcstol(portbuf, NULL, 10);
            wchar_t *colon = wcsrchr(a->host, L':'); /* accept host:port in host field */
            if (colon) { *colon = 0; a->port = (int)wcstol(colon + 1, NULL, 10); }
            if (a->port <= 0 || a->port > 65535) a->port = 5900;
            a->view_only  = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
            a->want_audio = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);
            wchar_t caw[1024] = L"";
            GetWindowTextW(GetDlgItem(hwnd, 205), caw, 1024);
            /* On overflow WideCharToMultiByte returns 0 and may leave the buffer
             * unterminated; treat a too-long path as "no CA" (which then fails the
             * TLS handshake closed) rather than passing an unterminated string. */
            if (!caw[0] || WideCharToMultiByte(CP_UTF8, 0, caw, -1, a->ca_file,
                                               (int)sizeof(a->ca_file), NULL, NULL) <= 0)
                a->ca_file[0] = 0;
            g_conn_ok = TRUE;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == 2) { g_conn_ok = FALSE; DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        g_conn_ok = FALSE; DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Modal connection prompt. Fills app->host/port/view_only/want_audio/ca_file.
 * Returns TRUE if the user chose Connect. */
static BOOL prompt_connection(ViewerApp *app, HINSTANCE hinst)
{
    static const wchar_t *cls = L"VncConnPrompt";
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = conn_proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered = TRUE;
    }

    g_conn_app = app;
    g_conn_ok = FALSE;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Connect to VNC server",
        WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 376, 276, NULL, NULL, hinst, NULL);
    if (!dlg)
        return FALSE;

#define CONN_MK(klass, text, style, x, y, w, h, id) \
    CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | (style), \
        x, y, w, h, dlg, (HMENU)(id), hinst, NULL)

    CONN_MK(L"STATIC", L"Host:", 0, 14, 16, 56, 18, 0);
    HWND host_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        74, 14, 274, 24, dlg, (HMENU)201, hinst, NULL);
    CONN_MK(L"STATIC", L"Port:", 0, 14, 50, 56, 18, 0);
    CONN_MK(L"EDIT", L"5900", WS_TABSTOP | ES_NUMBER | WS_BORDER, 74, 48, 80, 24, 202);
    CONN_MK(L"BUTTON", L"View only (no input sent)",
        WS_TABSTOP | BS_AUTOCHECKBOX, 74, 82, 260, 22, 203);
    CONN_MK(L"BUTTON", L"Enable audio (QEMU)",
        WS_TABSTOP | BS_AUTOCHECKBOX, 74, 108, 260, 22, 204);
    CONN_MK(L"STATIC", L"TLS CA file (optional, for VeNCrypt X509):",
        0, 14, 140, 340, 18, 0);
    CONN_MK(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 14, 160, 254, 24, 205);
    CONN_MK(L"BUTTON", L"Browse\x2026", WS_TABSTOP, 276, 160, 72, 24, 206);
    CONN_MK(L"BUTTON", L"Connect", WS_TABSTOP | BS_DEFPUSHBUTTON, 190, 204, 75, 28, 1);
    CONN_MK(L"BUTTON", L"Cancel", WS_TABSTOP, 273, 204, 75, 28, 2);
#undef CONN_MK

    SetWindowTextW(GetDlgItem(dlg, 202), L"5900");
    SetFocus(host_edit);

    MSG msg;
    BOOL got;
    while ((got = GetMessageW(&msg, NULL, 0, 0)) != 0) {
        if (got == -1)
            break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(dlg, WM_COMMAND, 1, 0); continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(dlg, WM_COMMAND, 2, 0); continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(dlg))
            break;
    }
    return g_conn_ok;
}

/* ---- entry point --------------------------------------------------------- */

static void parse_target(const wchar_t *arg, ViewerApp *app)
{
    wcsncpy_s(app->host, 256, arg, _TRUNCATE);
    app->port = 5900;
    wchar_t *colon = wcsrchr(app->host, L':');
    if (colon) {
        *colon = 0;
        app->port = (int)wcstol(colon + 1, NULL, 10);
    }
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;

    /* Parse the command line WITHOUT shell32 (see cmdline_to_wargv): shell32
     * would pull in user32 and break the worker's no-win32k confinement. */
    int argc = 0;
    wchar_t **argv = cmdline_to_wargv(GetCommandLineW(), &argc);
    if (!argv)
        return 1;

    /* ---- mode dispatch: one exe, three roles -------------------------------
     * These two modes are portable (char** argv) entry points; the GUI DLLs are
     * delay-loaded and never touched on these paths, so a --worker process runs
     * with user32/gdi32 unloaded and the win32k syscall filter intact. */
    if (wargv_has(argv, argc, L"--worker")) {
        int rc = run_utf8_mode(vnc_worker_main, argv, argc);
        free_wargv(argv);
        return rc;
    }
    if (wargv_has(argv, argc, L"--headless")) {
        /* Attach to the launching console (if any) so diagnostic output is
         * visible; this is the only mode that writes to stdout/stderr. */
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
            FILE *f;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
        }
        /* vnc_headless_main parses its target positionally as argv[1]; drop the
         * "--headless" mode token so it doesn't become the target. (The --worker
         * path needs no such strip: worker_main parses by key, not position.) */
        int w = 1;
        for (int i = 1; i < argc; i++)
            if (wcscmp(argv[i], L"--headless") != 0)
                argv[w++] = argv[i];
        argc = w;
        argv[argc] = NULL;
        int rc = run_utf8_mode(vnc_headless_main, argv, argc);
        free_wargv(argv);
        return rc;
    }

    /* ---- default: trusted UI/broker ---------------------------------------- */
    ZeroMemory(&g_app, sizeof(g_app));
    g_app.hinst = hinst; /* scale_mode defaults to 0 = fit (aspect-preserving) */
    g_app.port = 5900;
    BOOL debug = FALSE;
    BOOL have_host = FALSE;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != L'-' && !have_host) {
            parse_target(argv[i], &g_app);
            have_host = TRUE;
        } else if (!wcscmp(argv[i], L"--view-only"))
            g_app.view_only = TRUE;
        else if (!wcscmp(argv[i], L"--audio"))
            g_app.want_audio = TRUE;
        else if (!wcscmp(argv[i], L"--fullscreen"))
            g_app.want_fullscreen = TRUE;
        else if (!wcscmp(argv[i], L"--stretch"))
            g_app.scale_mode = 1;
        else if (!wcscmp(argv[i], L"--scale-1to1"))
            g_app.scale_mode = 2;
        else if (!wcscmp(argv[i], L"--debug"))
            debug = TRUE;
        else if (!wcscmp(argv[i], L"--ca") && i + 1 < argc) {
            if (WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_app.ca_file,
                                    (int)sizeof(g_app.ca_file), NULL, NULL) <= 0)
                g_app.ca_file[0] = 0; /* too long: don't pass an unterminated path */
        }
    }
    free_wargv(argv);

    /* No target on the command line: ask for one interactively. */
    if (!have_host) {
        if (!prompt_connection(&g_app, hinst))
            return 0; /* user cancelled */
    }

    /* Diagnostics (opt-in). The invocation summary is redacted: the target and
     * flags only, never a password (passwords never appear on argv). */
    char host_ascii[256];
    WideCharToMultiByte(CP_UTF8, 0, g_app.host, -1, host_ascii, sizeof(host_ascii),
                        NULL, NULL);
    char invocation[512];
    _snprintf_s(invocation, sizeof(invocation), _TRUNCATE,
        "host=%s port=%d view_only=%d audio=%d fullscreen=%d ca=%s scale=%d",
        host_ascii, g_app.port, g_app.view_only, g_app.want_audio,
        g_app.want_fullscreen, g_app.ca_file[0] ? "yes" : "no", g_app.scale_mode);
    diag_init(debug, invocation);
    diag_logf(DIAG_INFO, "vncviewer starting (%s)", invocation);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_app.shm_bytes = VIEWER_SHM_BYTES;
    g_app.shm = vnc_shm_create(g_app.shm_bytes, g_app.shm_name,
                               sizeof(g_app.shm_name));
    if (!g_app.shm) {
        MessageBoxW(NULL, L"Failed to create shared framebuffer.",
                    L"VNC Lightweight", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!viewer_register_class(hinst) || !viewer_create_window(&g_app, hinst)) {
        MessageBoxW(NULL, L"Failed to create window.", L"VNC Lightweight",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    if (g_app.want_fullscreen)
        SendMessageW(g_app.hwnd, WM_KEYDOWN, VK_F11, 0);

    /* Spawn the sandboxed worker + reader thread. */
    if (!app_start_session(&g_app)) {
        MessageBoxW(NULL,
            L"Failed to start the sandboxed connection worker.\n"
            L"The client will not run the RFB parser outside the sandbox.",
            L"VNC Lightweight", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg;
    BOOL got;
    while ((got = GetMessageW(&msg, NULL, 0, 0)) != 0) {
        if (got == -1)
            break; /* error: don't dispatch a garbage MSG */
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app_stop_session(&g_app);
    vnc_shm_close(g_app.shm);
    WSACleanup();

    if (diag_enabled()) {
        diag_logf(DIAG_INFO, "vncviewer exit");
        wchar_t wpath[MAX_PATH * 2], note[MAX_PATH * 2 + 128];
        MultiByteToWideChar(CP_UTF8, 0, diag_logpath(), -1, wpath, MAX_PATH * 2);
        _snwprintf_s(note, MAX_PATH * 2 + 128, _TRUNCATE,
            L"Diagnostic log written to:\n\n%s\n\n"
            L"It contains no passwords, clipboard text, or screen contents.",
            wpath);
        diag_close();
        MessageBoxW(NULL, note, L"VNC Lightweight \x2014 debug log",
                    MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}
