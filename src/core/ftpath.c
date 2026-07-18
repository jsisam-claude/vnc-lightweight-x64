#include "core/ftpath.h"

#include <string.h>

/* Windows reserved device names (case-insensitive, with or without extension). */
static bool is_reserved_device(const char *name)
{
    /* Compare up to the first '.' against the reserved set. */
    size_t base = 0;
    while (name[base] && name[base] != '.')
        base++;

    static const char *dev3[] = { "CON", "PRN", "AUX", "NUL" };
    if (base == 3) {
        for (size_t i = 0; i < 4; i++) {
            if ((name[0] == dev3[i][0] || name[0] == dev3[i][0] + 32) &&
                (name[1] == dev3[i][1] || name[1] == dev3[i][1] + 32) &&
                (name[2] == dev3[i][2] || name[2] == dev3[i][2] + 32))
                return true;
        }
    }
    /* COM1-9, LPT1-9 (4 chars). */
    if (base == 4) {
        char a = name[0], b = name[1], c = name[2], d = name[3];
        char A = (a >= 'a' && a <= 'z') ? a - 32 : a;
        char B = (b >= 'a' && b <= 'z') ? b - 32 : b;
        char C = (c >= 'a' && c <= 'z') ? c - 32 : c;
        if (((A == 'C' && B == 'O' && C == 'M') ||
             (A == 'L' && B == 'P' && C == 'T')) && d >= '1' && d <= '9')
            return true;
    }
    /* Console pseudo-devices CONIN$ / CONOUT$ ('$' is otherwise an allowed char). */
    static const char *devx[] = { "CONIN$", "CONOUT$" };
    for (size_t i = 0; i < 2; i++) {
        size_t L = strlen(devx[i]);
        if (base != L)
            continue;
        bool match = true;
        for (size_t j = 0; j < L; j++) {
            char c = name[j];
            char u = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            if (u != devx[i][j]) { match = false; break; }
        }
        if (match)
            return true;
    }
    return false;
}

bool ft_sanitize_remote_name(const char *in, char *out, size_t cap)
{
    if (cap)
        out[0] = '\0';
    if (!in || !out || cap == 0)
        return false;

    size_t n = strlen(in);
    if (n == 0 || n > FT_MAX_NAME || n >= cap)
        return false;

    /* Reject anything that is not a single plain component. */
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch < 0x20 || ch == 0x7F)      /* control chars */
            return false;
        if (ch == '/' || ch == '\\')      /* separators */
            return false;
        if (ch == ':')                    /* drive / ADS */
            return false;
        /* Characters Windows forbids in filenames. */
        if (ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
            ch == '>' || ch == '|')
            return false;
    }

    /* No "." or ".." components (the whole name IS one component here). */
    if (strcmp(in, ".") == 0 || strcmp(in, "..") == 0)
        return false;

    /* No trailing dot or space (Windows silently strips them -> mismatch). */
    if (in[n - 1] == '.' || in[n - 1] == ' ')
        return false;

    if (is_reserved_device(in))
        return false;

    memcpy(out, in, n);
    out[n] = '\0';
    return true;
}

const char *ft_basename(const char *path)
{
    if (!path)
        return "";
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}
