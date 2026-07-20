/*
 * png_write — a tiny PNG encoder for framebuffer screenshots.
 *
 * Portable C, no dependency beyond the already-vendored zlib. Takes the 32bpp
 * B,G,R,X framebuffer (our shm / Win32 DIB byte order — see viewer_window.c) and
 * writes an 8-bit truecolour PNG. Kept in src/core so it is unit-testable on
 * Linux (the headless client can dump one) and reused by the Windows screenshot.
 */
#ifndef VNC_CORE_PNG_WRITE_H
#define VNC_CORE_PNG_WRITE_H

#include <stdbool.h>
#include <stdint.h>

/* Write `width`x`height` 32bpp B,G,R,X pixels to `path` as an RGB PNG. The caller
 * MUST supply a buffer of at least width*height*4 bytes (the encoder trusts the
 * (w,h) <-> buffer-size contract). Returns false on bad args, allocation failure,
 * zlib error, or I/O error. `path` is interpreted by the CRT's char* fopen — on
 * Windows that is the ANSI code page, so prefer png_write_bgrx_w there. */
bool png_write_bgrx(const char *path, const uint8_t *bgrx, int width, int height);

#ifdef _WIN32
#include <wchar.h>
/* Wide-path variant for Windows: `path` is UTF-16, opened with _wfopen, so
 * non-ASCII paths (accented / CJK user folders) work — the char* fopen above
 * would mis-encode them under the ANSI code page (no UTF-8 manifest in this build). */
bool png_write_bgrx_w(const wchar_t *path, const uint8_t *bgrx, int width, int height);
#endif

#endif /* VNC_CORE_PNG_WRITE_H */
