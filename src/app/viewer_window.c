/*
 * viewer_window.c — window, framebuffer presentation, and the worker-event
 * reader thread (all in the trusted UI process).
 *
 * The shared framebuffer is 32bpp with byte order B,G,R,X on our little-endian
 * targets (the server format we negotiate: red shift 16, green 8, blue 0), which
 * is exactly what a BI_RGB 32bpp DIB expects, so StretchDIBits renders it with
 * no per-pixel conversion.
 */
#include "app/app.h"
#include "app/audio_waveout.h"
#include "app/diag.h"
#include "core/ftpath.h"
#include "ipc/protocol.h"

#include <windowsx.h> /* GET_X_LPARAM / GET_Y_LPARAM */
#include <shellapi.h> /* DragAcceptFiles / DragQueryFile */
#include <stdio.h>    /* _snwprintf_s */
#include <stdlib.h>
#include <string.h>

static const wchar_t *kClassName = L"VncLightweightViewer";

/* Compute where the framebuffer image is drawn within the client area, honoring
 * the scale mode. Used by both painting and mouse-coordinate mapping so they
 * always agree. */
static RECT compute_dest_rect(ViewerApp *app, int cw, int ch)
{
    RECT d = { 0, 0, cw, ch };
    if (app->fb_width <= 0 || app->fb_height <= 0)
        return d;
    if (app->scale_mode == 1) /* stretch: fill the whole client area */
        return d;
    if (app->scale_mode == 2) { /* 1:1, centered */
        int w = app->fb_width, h = app->fb_height;
        d.left = (cw - w) / 2; d.top = (ch - h) / 2;
        d.right = d.left + w; d.bottom = d.top + h;
        return d;
    }
    /* fit: preserve aspect ratio, letterbox. */
    double sx = (double)cw / app->fb_width, sy = (double)ch / app->fb_height;
    double s = sx < sy ? sx : sy;
    int w = (int)(app->fb_width * s + 0.5), h = (int)(app->fb_height * s + 0.5);
    d.left = (cw - w) / 2; d.top = (ch - h) / 2;
    d.right = d.left + w; d.bottom = d.top + h;
    return d;
}

static void toggle_fullscreen(ViewerApp *app)
{
    HWND hwnd = app->hwnd;
    if (!app->fullscreen) {
        app->windowed_style = GetWindowLongW(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &app->windowed_rect);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, app->windowed_style & ~(WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        app->fullscreen = TRUE;
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, app->windowed_style);
        SetWindowPos(hwnd, NULL, app->windowed_rect.left, app->windowed_rect.top,
                     app->windowed_rect.right - app->windowed_rect.left,
                     app->windowed_rect.bottom - app->windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        app->fullscreen = FALSE;
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

static void setup_dib(ViewerApp *app, int w, int h)
{
    ZeroMemory(&app->bmi, sizeof(app->bmi));
    app->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    app->bmi.bmiHeader.biWidth = w;
    app->bmi.bmiHeader.biHeight = -h; /* top-down */
    app->bmi.bmiHeader.biPlanes = 1;
    app->bmi.bmiHeader.biBitCount = 32;
    app->bmi.bmiHeader.biCompression = BI_RGB;
    app->fb_width = w;
    app->fb_height = h;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ViewerApp *app = (ViewerApp *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        AddClipboardFormatListener(hwnd); /* local clipboard -> server */
        DragAcceptFiles(hwnd, TRUE);      /* file drag-drop upload */
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        /* Capture + validate the dropped files. The actual upload transport is
         * the TightVNC file-transfer extension, which only a TightVNC/UltraVNC
         * guest server supports (QEMU's VNC has no file channel); it is not yet
         * wired. We validate names now so the security boundary (path-traversal
         * defense in ft_sanitize_remote_name / ft_basename) is already in place. */
        UINT valid = 0;
        for (UINT i = 0; i < count; i++) {
            wchar_t wpath[MAX_PATH];
            if (DragQueryFileW(drop, i, wpath, MAX_PATH)) {
                char path[MAX_PATH * 2];
                WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof(path), NULL, NULL);
                char safe[FT_MAX_NAME + 1];
                if (ft_sanitize_remote_name(ft_basename(path), safe, sizeof(safe)))
                    valid++;
            }
        }
        DragFinish(drop);
        wchar_t msg[256];
        _snwprintf_s(msg, 256, _TRUNCATE,
            L"%u file(s) ready to upload.\n\n"
            L"File transfer requires a TightVNC/UltraVNC server in the guest; "
            L"QEMU's built-in VNC has no file channel. The transport is not yet "
            L"implemented in this build.", valid);
        MessageBoxW(hwnd, msg, L"VNC Lightweight — File transfer",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    case WM_CLIPBOARDUPDATE:
        /* Ignore the echo from our own SetClipboardData (server text). */
        if (app->ignore_clip_update)
            app->ignore_clip_update = FALSE;
        else if (GetClipboardOwner() != hwnd)
            clipboard_to_server(app);
        return 0;

    case WM_APP_CURSOR: {
        uint8_t *blob = (uint8_t *)lp;
        if (blob) {
            /* blob = [u32 len][payload] marshaled by the reader thread. */
            uint32_t len;
            memcpy(&len, blob, 4);
            viewer_set_cursor(app, blob + 4, len);
            free(blob);
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && app->remote_cursor) {
            SetCursor(app->remote_cursor);
            return TRUE;
        }
        break;
    case WM_APP_RESIZE:
        diag_logf(DIAG_INFO, "framebuffer resize -> %dx%d", (int)wp, (int)lp);
        setup_dib(app, (int)wp, (int)lp);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_APP_UPDATE: {
        vnc_ipc_rect *r = (vnc_ipc_rect *)lp;
        if (r) {
            /* The rect is in framebuffer coordinates but the image is stretched
             * to the client area, so scale it (with a 1px margin) before
             * invalidating, or the wrong region repaints when sizes differ. */
            RECT cr; GetClientRect(hwnd, &cr);
            int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
            if (app->fb_width > 0 && app->fb_height > 0 && cw > 0 && ch > 0) {
                RECT rc;
                rc.left   = (LONG)((int64_t)r->x * cw / app->fb_width) - 1;
                rc.top    = (LONG)((int64_t)r->y * ch / app->fb_height) - 1;
                rc.right  = (LONG)((int64_t)(r->x + r->w) * cw / app->fb_width) + 2;
                rc.bottom = (LONG)((int64_t)(r->y + r->h) * ch / app->fb_height) + 2;
                InvalidateRect(hwnd, &rc, FALSE);
            } else {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            free(r);
        }
        return 0;
    }
    case WM_APP_CUTTEXT: {
        /* lp -> heap {uint32_t len; bytes} */
        uint8_t *blob = (uint8_t *)lp;
        if (blob) {
            uint32_t len;
            memcpy(&len, blob, 4);
            clipboard_from_server(app, (const char *)(blob + 4), len);
            free(blob);
        }
        return 0;
    }
    case WM_APP_PWREQ:
        app_request_password(app);
        return 0;

    case WM_APP_LED: {
        /* QEMU LED state: bit0=Scroll, bit1=Num, bit2=Caps. Reflect it in the
         * title (non-intrusively; we don't force the local keyboard LEDs). */
        unsigned s = (unsigned)wp;
        wchar_t title[128];
        _snwprintf_s(title, 128, _TRUNCATE, L"VNC Lightweight  [%s%s%s]",
                     (s & 4) ? L"CAPS " : L"", (s & 2) ? L"NUM " : L"",
                     (s & 1) ? L"SCROLL" : L"");
        SetWindowTextW(hwnd, title);
        return 0;
    }

    case WM_APP_STATUS:
        diag_logf(DIAG_INFO, "status: %s",
                  (int)wp == VNC_STATUS_CONNECTED ? "connected" :
                  (int)wp == VNC_STATUS_AUTH_FAILED ? "auth-failed" :
                  (int)wp == VNC_STATUS_CONNECT_FAILED ? "connect-failed" :
                  "disconnected");
        if ((int)wp == VNC_STATUS_CONNECTED) {
            app->connected = TRUE;
        } else if ((int)wp == VNC_STATUS_DISCONNECTED ||
                   (int)wp == VNC_STATUS_CONNECT_FAILED ||
                   (int)wp == VNC_STATUS_AUTH_FAILED) {
            BOOL was_connected = app->connected;
            app->connected = FALSE;
            const wchar_t *why =
                (int)wp == VNC_STATUS_AUTH_FAILED ? L"Authentication failed." :
                (int)wp == VNC_STATUS_CONNECT_FAILED ? L"Could not connect to the server." :
                L"Disconnected from the server.";
            wchar_t prompt[256];
            _snwprintf_s(prompt, 256, _TRUNCATE, L"%s\n\nReconnect?", why);
            /* Tear the old session down (the reader thread that posted this has
             * already exited), then offer to reconnect. */
            app_stop_session(app);
            if (MessageBoxW(hwnd, prompt, L"VNC Lightweight",
                            MB_YESNO | MB_ICONWARNING) == IDYES &&
                app_start_session(app)) {
                (void)was_connected;
            } else {
                DestroyWindow(hwnd);
            }
        }
        return 0;

    case WM_PAINT: {
        static int logged_first_frame = 0;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        if (app->fb_width > 0 && app->fb_height > 0 && app->shm) {
            RECT d = compute_dest_rect(app, cw, ch);
            if (!logged_first_frame) {
                logged_first_frame = 1;
                diag_logf(DIAG_INFO, "first frame painted (%dx%d -> %dx%d)",
                          app->fb_width, app->fb_height, cw, ch);
            }
            /* Letterbox margins in black (only when the image doesn't fill). */
            if (d.left > 0 || d.top > 0 || d.right < cw || d.bottom < ch) {
                HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
                RECT full = { 0, 0, cw, ch };
                FillRect(hdc, &full, black);
            }
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchDIBits(hdc, d.left, d.top, d.right - d.left, d.bottom - d.top,
                          0, 0, app->fb_width, app->fb_height,
                          vnc_shm_pixels(app->shm), &app->bmi,
                          DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; /* we paint every pixel; skip flicker */

    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp == VK_F11) { toggle_fullscreen(app); return 0; }
        /* Ctrl+Alt+F toggles fullscreen too (F11 may be grabbed by the guest). */
        if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000) &&
            (GetKeyState(VK_MENU) & 0x8000)) { toggle_fullscreen(app); return 0; }
        input_key(app, wp, lp, TRUE);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        input_key(app, wp, lp, FALSE);
        return 0;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        /* Map window coords to framebuffer coords through the same dest rect the
         * paint uses, so letterbox/1:1 modes map correctly. */
        RECT rc; GetClientRect(hwnd, &rc);
        RECT d = compute_dest_rect(app, rc.right - rc.left, rc.bottom - rc.top);
        int dw = d.right - d.left, dh = d.bottom - d.top;
        if (dw > 0 && dh > 0 && app->fb_width > 0) {
            x = (int)((int64_t)(x - d.left) * app->fb_width / dw);
            y = (int)((int64_t)(y - d.top) * app->fb_height / dh);
            if (x < 0) x = 0; if (x >= app->fb_width) x = app->fb_width - 1;
            if (y < 0) y = 0; if (y >= app->fb_height) y = app->fb_height - 1;
        }
        input_pointer(app, x, y, msg, wp);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        if (app->remote_cursor) DestroyCursor(app->remote_cursor);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Build a Windows cursor from an EVT_CURSOR blob and make it current. The blob
 * is a vnc_ipc_cursor header followed by width*height BGRA pixels (alpha carries
 * the mask). All fields are re-validated here (untrusted worker). */
void viewer_set_cursor(ViewerApp *app, const uint8_t *blob, unsigned len)
{
    if (len < sizeof(vnc_ipc_cursor))
        return;
    vnc_ipc_cursor hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    unsigned w = hdr.width, h = hdr.height;
    if (w == 0 || h == 0 || w > 256 || h > 256)
        return;
    size_t need = (size_t)w * h * 4;
    if (len - sizeof(vnc_ipc_cursor) < need)
        return;
    const uint8_t *bgra = blob + sizeof(vnc_ipc_cursor);

    /* 32bpp top-down color bitmap holding the ARGB pixels. */
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC hdc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!color)
        return;
    memcpy(bits, bgra, need);

    /* AND mask must exist and be all-zero so the 32bpp color alpha governs
     * transparency (CreateBitmap with NULL leaves bits undefined, so zero it). */
    size_t mask_stride = (((size_t)w + 15) / 16) * 2; /* 1bpp, WORD-aligned rows */
    uint8_t *mask_bits = calloc(mask_stride * h, 1);
    HBITMAP mask = mask_bits ? CreateBitmap((int)w, (int)h, 1, 1, mask_bits) : NULL;
    free(mask_bits);
    if (!mask) { DeleteObject(color); return; }

    ICONINFO ii = {0};
    ii.fIcon = FALSE; /* cursor */
    ii.xHotspot = (DWORD)(hdr.xhot < 0 ? 0 : hdr.xhot);
    ii.yHotspot = (DWORD)(hdr.yhot < 0 ? 0 : hdr.yhot);
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HCURSOR cur = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    if (!cur)
        return;

    if (app->remote_cursor)
        DestroyCursor(app->remote_cursor);
    app->remote_cursor = cur;
    SetCursor(cur);
    diag_logf(DIAG_DEBUG, "cursor set %ux%u", w, h);
}

ATOM viewer_register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc);
}

HWND viewer_create_window(ViewerApp *app, HINSTANCE hinst)
{
    HWND hwnd = CreateWindowExW(
        0, kClassName, L"VNC Lightweight",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
        NULL, NULL, hinst, app);
    app->hwnd = hwnd;
    return hwnd;
}

/* Remove any queued WM_APP_* messages (freeing heap payloads) so stale events
 * from a torn-down session cannot leak or disturb a reconnected one. Call only
 * after the posting reader thread has been joined. */
void viewer_drain_messages(HWND hwnd)
{
    MSG m;
    while (PeekMessageW(&m, hwnd, WM_APP_RESIZE, WM_APP_LED, PM_REMOVE)) {
        if ((m.message == WM_APP_UPDATE || m.message == WM_APP_CUTTEXT ||
             m.message == WM_APP_CURSOR) && m.lParam)
            free((void *)m.lParam);
    }
}

/* Reader thread: consume worker events and marshal them to the UI thread. */
DWORD WINAPI viewer_reader_thread(LPVOID arg)
{
    ViewerApp *app = (ViewerApp *)arg;
    for (;;) {
        uint32_t type = 0, len = 0;
        static uint8_t buf[VNC_IPC_MAX_PAYLOAD];
        int r = vnc_channel_recv(&app->ch, &type, buf, sizeof(buf), &len);
        if (r <= 0)
            break; /* worker gone or malformed frame -> tear down */

        switch (type) {
        case VNC_EVT_HELLO:
            break;
        case VNC_EVT_STATUS:
            if (len == sizeof(vnc_ipc_status))
                PostMessageW(app->hwnd, WM_APP_STATUS, buf[0], 0);
            break;
        case VNC_EVT_RESIZE:
            if (len == sizeof(vnc_ipc_resize)) {
                vnc_ipc_resize *rz = (void *)buf;
                PostMessageW(app->hwnd, WM_APP_RESIZE, rz->width, rz->height);
            }
            break;
        case VNC_EVT_UPDATE:
            if (len == sizeof(vnc_ipc_rect)) {
                vnc_ipc_rect *copy = malloc(sizeof(*copy));
                if (copy) { *copy = *(vnc_ipc_rect *)buf;
                    PostMessageW(app->hwnd, WM_APP_UPDATE, 0, (LPARAM)copy); }
            }
            break;
        case VNC_EVT_CUT_TEXT: {
            uint8_t *blob = malloc(4 + (size_t)len);
            if (blob) { memcpy(blob, &len, 4); memcpy(blob + 4, buf, len);
                PostMessageW(app->hwnd, WM_APP_CUTTEXT, 0, (LPARAM)blob); }
            break;
        }
        case VNC_EVT_CURSOR: {
            uint8_t *blob = malloc(4 + (size_t)len);
            if (blob) { memcpy(blob, &len, 4); memcpy(blob + 4, buf, len);
                PostMessageW(app->hwnd, WM_APP_CURSOR, 0, (LPARAM)blob); }
            break;
        }
        case VNC_EVT_LED:
            if (len == sizeof(vnc_ipc_led))
                PostMessageW(app->hwnd, WM_APP_LED, buf[0], 0);
            break;

        /* Audio is handled directly on this reader thread (off the GUI thread)
         * to keep playback latency low. */
        case VNC_EVT_AUDIO_FORMAT:
            if (len == sizeof(vnc_ipc_audio_cfg)) {
                vnc_ipc_audio_cfg *a = (void *)buf;
                if (app->audio) waveout_destroy(app->audio);
                app->audio = waveout_create(a->sample_format, a->channels,
                                            a->frequency);
                diag_logf(DIAG_INFO, "audio format fmt=%u ch=%u freq=%u waveOut=%s",
                          a->sample_format, a->channels, a->frequency,
                          app->audio ? "open" : "FAILED");
            }
            break;
        case VNC_EVT_AUDIO_DATA:
            if (app->audio) waveout_feed(app->audio, buf, len);
            break;
        case VNC_EVT_AUDIO_END:
            if (app->audio) waveout_reset(app->audio);
            break;
        case VNC_EVT_PASSWORD_REQ:
            PostMessageW(app->hwnd, WM_APP_PWREQ, 0, 0);
            break;
        case VNC_EVT_LOG:
            /* buf[0]=level (vnc_log_level), rest=message (not NUL-terminated).
             * These carry the libvncclient/TLS protocol timeline — route them to
             * the diagnostic log. */
            if (diag_enabled() && len >= 1) {
                char m[900];
                uint32_t mlen = len - 1;
                if (mlen >= sizeof(m)) mlen = sizeof(m) - 1;
                memcpy(m, buf + 1, mlen);
                m[mlen] = '\0';
                /* vnc_log_level: 0=ERROR,1=WARN,2=INFO,3=DEBUG -> diag_level. */
                diag_level dl = buf[0] <= DIAG_DEBUG ? (diag_level)buf[0] : DIAG_INFO;
                diag_logf(dl, "worker: %s", m);
            }
            break;
        default:
            break;
        }
    }
    /* Signal disconnect so the UI can close. */
    PostMessageW(app->hwnd, WM_APP_STATUS, VNC_STATUS_DISCONNECTED, 0);
    return 0;
}
