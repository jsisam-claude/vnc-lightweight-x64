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

#include <shellapi.h>
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
    char utf8[512];
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
        WaitForSingleObject(app->reader_thread, 1000);
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

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) {
        MessageBoxW(NULL, L"Usage: vncviewer HOST[:PORT] [--view-only]",
                    L"VNC Lightweight", MB_OK | MB_ICONINFORMATION);
        return 2;
    }
    ZeroMemory(&g_app, sizeof(g_app));
    parse_target(argv[1], &g_app);
    for (int i = 2; i < argc; i++) {
        if (!wcscmp(argv[i], L"--view-only"))
            g_app.view_only = TRUE;
        else if (!wcscmp(argv[i], L"--audio"))
            g_app.want_audio = TRUE;
        else if (!wcscmp(argv[i], L"--fullscreen"))
            g_app.want_fullscreen = TRUE;
        else if (!wcscmp(argv[i], L"--stretch"))
            g_app.scale_mode = 1;
        else if (!wcscmp(argv[i], L"--scale-1to1"))
            g_app.scale_mode = 2;
        else if (!wcscmp(argv[i], L"--ca") && i + 1 < argc)
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_app.ca_file,
                                sizeof(g_app.ca_file), NULL, NULL);
    }
    LocalFree(argv);
    g_app.hinst = hinst; /* scale_mode defaults to 0 = fit (aspect-preserving) */

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
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app_stop_session(&g_app);
    vnc_shm_close(g_app.shm);
    WSACleanup();
    return 0;
}
