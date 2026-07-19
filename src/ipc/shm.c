#include "ipc/shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vnc_shm {
    void  *base;
    size_t total_bytes;
#ifdef _WIN32
    void *handle; /* HANDLE from CreateFileMapping/OpenFileMapping */
#else
    int   fd;
    char  name[64];
    bool  owner;
#endif
};

static size_t total_size(size_t max_pixel_bytes)
{
    return VNC_SHM_PIXELS_OFF + max_pixel_bytes;
}

#ifdef _WIN32
/* ------------------------------ Windows -------------------------------- */
#include <windows.h>

vnc_shm *vnc_shm_create(size_t max_pixel_bytes, char *name_out, size_t name_cap)
{
    size_t total = total_size(max_pixel_bytes);
    vnc_shm *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    /* Unique, unpredictable name in the Local namespace. The AppContainer
     * worker is granted access to this specific object by name via its SID at
     * spawn time (see sandbox_win32.c). */
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    char name[64];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "Local\\vncfb-%lu-%llx",
                (unsigned long)GetCurrentProcessId(),
                (unsigned long long)pc.QuadPart);

    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  (DWORD)((uint64_t)total >> 32),
                                  (DWORD)(total & 0xFFFFFFFFu), name);
    if (!h) {
        free(s);
        return NULL;
    }
    void *base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!base) {
        CloseHandle(h);
        free(s);
        return NULL;
    }
    memset(base, 0, total);
    ((vnc_shm_header *)base)->magic = VNC_SHM_MAGIC;

    s->base = base;
    s->total_bytes = total;
    s->handle = h;
    strncpy_s(name_out, name_cap, name, _TRUNCATE);
    return s;
}

vnc_shm *vnc_shm_open(const char *name, size_t max_pixel_bytes)
{
    size_t total = total_size(max_pixel_bytes);
    vnc_shm *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h) {
        free(s);
        return NULL;
    }
    void *base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!base) {
        CloseHandle(h);
        free(s);
        return NULL;
    }
    if (((vnc_shm_header *)base)->magic != VNC_SHM_MAGIC) {
        UnmapViewOfFile(base);
        CloseHandle(h);
        free(s);
        return NULL;
    }
    s->base = base;
    s->total_bytes = total;
    s->handle = h;
    return s;
}

void vnc_shm_close(vnc_shm *s)
{
    if (!s)
        return;
    if (s->base)
        UnmapViewOfFile(s->base);
    if (s->handle)
        CloseHandle((HANDLE)s->handle);
    free(s);
}

void vnc_shm_unlink(const char *name) { (void)name; } /* refcounted on Windows */

void *vnc_shm_native_handle(vnc_shm *s) { return s ? s->handle : NULL; }

vnc_shm *vnc_shm_from_handle(void *handle, size_t max_pixel_bytes)
{
    size_t total = total_size(max_pixel_bytes);
    vnc_shm *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    void *base = MapViewOfFile((HANDLE)handle, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!base) {
        free(s);
        return NULL;
    }
    if (((vnc_shm_header *)base)->magic != VNC_SHM_MAGIC) {
        UnmapViewOfFile(base);
        free(s);
        return NULL;
    }
    s->base = base;
    s->total_bytes = total;
    s->handle = (HANDLE)handle; /* we own it now */
    return s;
}

#else
/* ------------------------------ POSIX ---------------------------------- */
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

vnc_shm *vnc_shm_create(size_t max_pixel_bytes, char *name_out, size_t name_cap)
{
    size_t total = total_size(max_pixel_bytes);
    vnc_shm *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    /* Unique name; PID + address entropy is enough for a same-host test. */
    snprintf(s->name, sizeof(s->name), "/vncfb-%d-%p", (int)getpid(), (void *)s);
    s->owner = true;

    int fd = shm_open(s->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        free(s);
        return NULL;
    }
    if (ftruncate(fd, (off_t)total) != 0) {
        close(fd);
        shm_unlink(s->name);
        free(s);
        return NULL;
    }
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        shm_unlink(s->name);
        free(s);
        return NULL;
    }
    memset(base, 0, total);
    ((vnc_shm_header *)base)->magic = VNC_SHM_MAGIC;

    s->base = base;
    s->total_bytes = total;
    s->fd = fd;
    if (name_out && name_cap) { /* guard: name_cap-1 would underflow at 0 */
        strncpy(name_out, s->name, name_cap - 1);
        name_out[name_cap - 1] = '\0';
    }
    return s;
}

vnc_shm *vnc_shm_open(const char *name, size_t max_pixel_bytes)
{
    size_t total = total_size(max_pixel_bytes);
    vnc_shm *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->owner = false;

    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        free(s);
        return NULL;
    }
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        free(s);
        return NULL;
    }
    if (((vnc_shm_header *)base)->magic != VNC_SHM_MAGIC) {
        munmap(base, total);
        close(fd);
        free(s);
        return NULL;
    }
    s->base = base;
    s->total_bytes = total;
    s->fd = fd;
    return s;
}

void vnc_shm_close(vnc_shm *s)
{
    if (!s)
        return;
    if (s->base && s->base != MAP_FAILED)
        munmap(s->base, s->total_bytes);
    if (s->fd >= 0)
        close(s->fd);
    if (s->owner)
        shm_unlink(s->name);
    free(s);
}

void vnc_shm_unlink(const char *name) { shm_unlink(name); }

#endif

uint8_t *vnc_shm_pixels(vnc_shm *s)
{
    return (uint8_t *)s->base + VNC_SHM_PIXELS_OFF;
}

vnc_shm_header *vnc_shm_hdr(vnc_shm *s)
{
    return (vnc_shm_header *)s->base;
}

size_t vnc_shm_capacity(const vnc_shm *s)
{
    return s->total_bytes - VNC_SHM_PIXELS_OFF;
}
