/*
 * ftpath — filename/path sanitization for the file-transfer feature.
 *
 * File transfer is the classic path-traversal CVE area in VNC clients, so the
 * security-critical decisions live here in portable, heavily unit-tested code
 * (tests/ftpath_test), independent of the (Windows, server-gated) transport.
 *
 * Rules for a name RECEIVED FROM A SERVER (untrusted) before it is used as a
 * local download filename: it must be a single, plain path component — no
 * directory separators, no "." / ".." components, no drive letters or UNC, no
 * Windows reserved device names, no control characters, no trailing dots or
 * spaces, non-empty, and within a length bound.
 */
#ifndef VNC_CORE_FTPATH_H
#define VNC_CORE_FTPATH_H

#include <stdbool.h>
#include <stddef.h>

#define FT_MAX_NAME 255

/* Validate an untrusted server-supplied filename and copy the safe basename to
 * out (capacity cap). Returns true iff the name is a safe single component.
 * On false, out is set to an empty string. */
bool ft_sanitize_remote_name(const char *in, char *out, size_t cap);

/* Extract the basename of a local (client-side) path for use as the transferred
 * name. Handles both '/' and '\\' separators. Returns a pointer into `path`. */
const char *ft_basename(const char *path);

#endif /* VNC_CORE_FTPATH_H */
