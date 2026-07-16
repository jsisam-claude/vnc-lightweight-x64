/*
 * clipboard_win32.c — clipboard bridge (trusted UI process).
 *
 * M2: server cut-text -> local clipboard. M3 adds the local -> server direction
 * and the RFB Extended Clipboard (what QEMU speaks). Server text is untrusted:
 * length is already capped by the IPC layer; we treat it as UTF-8 and convert
 * defensively without assuming NUL-termination.
 */
#include "app/app.h"

void clipboard_from_server(ViewerApp *app, const char *text, unsigned len)
{
    if (len == 0)
        return;
    int wneed = MultiByteToWideChar(CP_UTF8, 0, text, (int)len, NULL, 0);
    /* Buffer must also accommodate the Latin-1 fallback, which writes `len`
     * wchars — size for the larger of the two so neither path can overflow. */
    size_t cap = (size_t)((wneed > (int)len) ? wneed : (int)len) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, cap * sizeof(wchar_t));
    if (!h)
        return;
    wchar_t *dst = (wchar_t *)GlobalLock(h);
    int got = (wneed > 0)
                ? MultiByteToWideChar(CP_UTF8, 0, text, (int)len, dst, wneed)
                : 0;
    if (got <= 0) {
        /* Not valid UTF-8: fall back to Latin-1 (classic RFB cut text). */
        for (unsigned i = 0; i < len; i++)
            dst[i] = (unsigned char)text[i];
        got = (int)len;
    }
    dst[got] = L'\0';
    GlobalUnlock(h);

    BOOL placed = FALSE;
    if (OpenClipboard(app->hwnd)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, h)) /* clipboard owns h now */
            placed = TRUE;
        CloseClipboard();
    }
    if (!placed)
        GlobalFree(h);
}
