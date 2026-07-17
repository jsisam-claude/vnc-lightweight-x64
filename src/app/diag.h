/*
 * diag — opt-in, privacy-respecting diagnostic logging for the Windows client.
 *
 * OFF by default. Enabled with `--debug` or the VNC_DEBUG environment variable,
 * at which point a single timestamped log file is written to %TEMP% (path is
 * printed to the log header and via a message box on exit) that a user can paste
 * back when reporting a problem.
 *
 * PRIVACY: the log deliberately never contains passwords, clipboard text, or any
 * screen/framebuffer pixels — only lengths, dimensions, protocol milestones, and
 * Win32/SSPI status codes. The server host:port IS included (it is needed to
 * diagnose connection issues and is the user's own target); the header says so
 * so the user can redact it before sharing if they wish.
 */
#ifndef VNC_APP_DIAG_H
#define VNC_APP_DIAG_H

#include <stdarg.h>

typedef enum { DIAG_ERROR = 0, DIAG_WARN, DIAG_INFO, DIAG_DEBUG } diag_level;

/* Enable logging if `force` or VNC_DEBUG is set. `invocation` is a redacted,
 * one-line summary of how the app was launched (no secrets). Safe to call once.
 * Returns nonzero if logging is enabled. */
int  diag_init(int force, const char *invocation);

int  diag_enabled(void);

/* Log a line (printf-style). No-op when disabled. Never pass secret content. */
void diag_logf(diag_level level, const char *fmt, ...);
void diag_log(diag_level level, const char *msg);

/* Format a Win32 GetLastError()-style code with its text: "ctx: err N (text)". */
void diag_win32(const char *ctx, unsigned long err);

/* Absolute path of the log file (empty string if disabled). */
const char *diag_logpath(void);

void diag_close(void);

#endif /* VNC_APP_DIAG_H */
