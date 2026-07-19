/*
 * clipboard_win32.c — clipboard bridge (trusted UI process).
 *
 * M2: server cut-text -> local clipboard. M3 adds the local -> server direction
 * and the RFB Extended Clipboard (what QEMU speaks). Server text is untrusted:
 * length is already capped by the IPC layer; we treat it as UTF-8 and convert
 * defensively without assuming NUL-termination.
 */
#include "app/app.h"
#include "app/diag.h"

#include <stdlib.h> /* malloc/free — else implicit int truncates the ptr on x64 */

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
    if (!dst) {          /* lock failed: don't dereference NULL */
        GlobalFree(h);
        return;
    }
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
    else
        app->ignore_clip_update = TRUE; /* suppress the echo back to the server */
    diag_logf(DIAG_DEBUG, "clipboard server->local: %u bytes (%s)", len,
              placed ? "set" : "failed"); /* length only, never the text */
}

/* Read the local clipboard (Unicode text) and forward it to the server as a
 * cut-text command. Text is UTF-8 encoded and capped by the IPC layer. */
void clipboard_to_server(ViewerApp *app)
{
    if (app->view_only)
        return;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return;
    if (!OpenClipboard(app->hwnd))
        return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w) {
            int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if (need > 1 && need <= (1 << 20)) {
                char *utf8 = malloc((size_t)need);
                if (utf8) {
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, need, NULL, NULL);
                    /* -1 to drop the NUL terminator from the wire length. */
                    vnc_channel_send(&app->ch, VNC_CMD_CUT_TEXT, utf8,
                                     (uint32_t)(need - 1));
                    diag_logf(DIAG_DEBUG, "clipboard local->server: %d bytes",
                              need - 1); /* length only, never the text */
                    free(utf8);
                }
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}
