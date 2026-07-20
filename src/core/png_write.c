#include "core/png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Write one PNG chunk: length + type + data + CRC32(type+data). */
static bool write_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8)
        return false;
    if (len && fwrite(data, 1, len, f) != len)
        return false;
    uLong crc = crc32(0L, (const Bytef *)type, 4);
    if (len)
        crc = crc32(crc, data, len);
    uint8_t c[4];
    put_be32(c, (uint32_t)crc);
    return fwrite(c, 1, 4, f) == 4;
}

/* Encode `bgrx` (width*height*4 bytes) into the already-open binary file `f`,
 * then close it. `f`, `bgrx` non-NULL and width/height > 0 are the caller's job. */
static bool encode_to_file(FILE *f, const uint8_t *bgrx, int width, int height)
{
    /* Raw image: each row is a 1-byte filter tag (0 = none) + width*3 RGB. */
    size_t rowbytes = (size_t)width * 3u + 1u;
    if (rowbytes / 3u < (size_t)width)      /* width*3 overflow (32-bit size_t) */
        { fclose(f); return false; }
    size_t raw_len = rowbytes * (size_t)height;
    if (raw_len / rowbytes != (size_t)height) /* rowbytes*height overflow */
        { fclose(f); return false; }
    /* zlib's uLong is 32-bit on LLP64 (Win64); refuse a raw image that would not
     * fit so compress2's length argument cannot silently truncate. (Unreachable
     * via VNC: the framebuffer is capped far below 4 GiB.) */
    if (raw_len > 0xFFFFFFFFu)
        { fclose(f); return false; }

    uint8_t *raw = malloc(raw_len);
    if (!raw)
        { fclose(f); return false; }
    for (int y = 0; y < height; y++) {
        uint8_t *row = raw + (size_t)y * rowbytes;
        row[0] = 0; /* filter: none */
        const uint8_t *src = bgrx + (size_t)y * (size_t)width * 4u;
        uint8_t *dst = row + 1;
        for (int x = 0; x < width; x++) {
            dst[0] = src[2]; /* R (BGRX -> RGB) */
            dst[1] = src[1]; /* G */
            dst[2] = src[0]; /* B */
            dst += 3; src += 4;
        }
    }

    uLong bound = compressBound((uLong)raw_len);
    uint8_t *comp = malloc(bound);
    if (!comp) { free(raw); fclose(f); return false; }
    uLongf comp_len = bound;
    int zr = compress2(comp, &comp_len, raw, (uLong)raw_len, Z_BEST_SPEED);
    free(raw);
    /* IDAT length is a uint32 chunk field; refuse a stream that wouldn't fit
     * rather than truncate it (also unreachable at VNC framebuffer sizes). */
    if (zr != Z_OK || comp_len > 0xFFFFFFFFu) { free(comp); fclose(f); return false; }

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)width);
    put_be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour RGB */
    ihdr[10] = 0; /* compression: deflate */
    ihdr[11] = 0; /* filter: adaptive */
    ihdr[12] = 0; /* interlace: none */

    bool ok = fwrite(sig, 1, 8, f) == 8 &&
              write_chunk(f, "IHDR", ihdr, sizeof(ihdr)) &&
              write_chunk(f, "IDAT", comp, (uint32_t)comp_len) &&
              write_chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0)
        ok = false;
    free(comp);
    return ok;
}

bool png_write_bgrx(const char *path, const uint8_t *bgrx, int width, int height)
{
    if (!path || !bgrx || width <= 0 || height <= 0)
        return false;
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    return encode_to_file(f, bgrx, width, height);
}

#ifdef _WIN32
bool png_write_bgrx_w(const wchar_t *path, const uint8_t *bgrx, int width, int height)
{
    if (!path || !bgrx || width <= 0 || height <= 0)
        return false;
    FILE *f = _wfopen(path, L"wb");
    if (!f)
        return false;
    return encode_to_file(f, bgrx, width, height);
}
#endif
