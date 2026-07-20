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

/* Write `width`x`height` 32bpp B,G,R,X pixels to `path` as an RGB PNG.
 * Returns false on bad args, allocation failure, zlib error, or I/O error. */
bool png_write_bgrx(const char *path, const uint8_t *bgrx, int width, int height);

#endif /* VNC_CORE_PNG_WRITE_H */
