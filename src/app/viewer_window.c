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
#include "ipc/protocol.h"

#include <windowsx.h> /* GET_X_LPARAM / GET_Y_LPARAM */
#include <stdio.h>    /* _snwprintf_s */
#include <stdlib.h>
#include <string.h>

static const wchar_t *kClassName = L"VncLightweightViewer";

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
        if ((int)wp == VNC_STATUS_CONNECTED) app->connected = TRUE;
        else if ((int)wp == VNC_STATUS_DISCONNECTED ||
                 (int)wp == VNC_STATUS_CONNECT_FAILED ||
                 (int)wp == VNC_STATUS_AUTH_FAILED) {
            app->connected = FALSE;
            /* M7 adds a reconnect prompt; for now close on disconnect. */
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (app->fb_width > 0 && app->fb_height > 0 && app->shm) {
            RECT rc; GetClientRect(hwnd, &rc);
            int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchDIBits(hdc, 0, 0, cw, ch,
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
        /* Map window coords back to framebuffer coords (we stretch to fit). */
        RECT rc; GetClientRect(hwnd, &rc);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        if (cw > 0 && ch > 0 && app->fb_width > 0) {
            x = (int)((int64_t)x * app->fb_width / cw);
            y = (int)((int64_t)y * app->fb_height / ch);
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
            /* buf[0]=level, rest=message. Silently dropped for now. */
            break;
        default:
            break;
        }
    }
    /* Signal disconnect so the UI can close. */
    PostMessageW(app->hwnd, WM_APP_STATUS, VNC_STATUS_DISCONNECTED, 0);
    return 0;
}
