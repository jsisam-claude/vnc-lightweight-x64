/*
 * Shared framebuffer mapping (ipc/shm).
 *
 * The decoded 32bpp framebuffer is the one large object shared between worker
 * and UI. It travels through a named shared mapping rather than the message
 * channel: the worker writes decoded pixels, the UI blits them. A small header
 * at the start of the mapping carries the agreed dimensions so both sides stay
 * in sync across resizes.
 *
 * Portable: Win32 file mapping (product) and POSIX shm_open/mmap (Linux test).
 */
#ifndef VNC_IPC_SHM_H
#define VNC_IPC_SHM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define VNC_SHM_MAGIC 0x53484D31u /* "SHM1" */

/* Header lives at offset 0 of the mapping; pixels follow at VNC_SHM_PIXELS_OFF. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t generation; /* bumped by worker on each resize; UI re-reads dims */
} vnc_shm_header;
#pragma pack(pop)

#define VNC_SHM_PIXELS_OFF 64u /* pixels start here (header padded to 64) */

typedef struct vnc_shm vnc_shm;

/* Create (owner=UI) a mapping large enough for max_bytes of pixel data, with a
 * unique name written into name_out (>= 64 chars). Returns NULL on failure. */
vnc_shm *vnc_shm_create(size_t max_pixel_bytes, char *name_out, size_t name_cap);

/* Open (worker) an existing mapping by name. max_pixel_bytes must match the
 * creator's value (the worker receives it via argv). */
vnc_shm *vnc_shm_open(const char *name, size_t max_pixel_bytes);

/* Base of the pixel region (past the header). */
uint8_t *vnc_shm_pixels(vnc_shm *s);
vnc_shm_header *vnc_shm_hdr(vnc_shm *s);

/* Capacity of the pixel region in bytes. */
size_t vnc_shm_capacity(const vnc_shm *s);

void vnc_shm_close(vnc_shm *s);

#ifdef _WIN32
/* Native file-mapping HANDLE, so the sandbox can grant the AppContainer SID
 * access to this specific object and duplicate it as inheritable. Returns NULL
 * if unavailable. */
void *vnc_shm_native_handle(vnc_shm *s);

/* Map an already-open file-mapping HANDLE (e.g. one inherited from the parent).
 * Used by the sandboxed worker, which cannot open the mapping by name because an
 * AppContainer resolves names in its own object namespace. Takes ownership of
 * the handle (closes it on vnc_shm_close). */
vnc_shm *vnc_shm_from_handle(void *handle, size_t max_pixel_bytes);
#endif

#endif /* VNC_IPC_SHM_H */
