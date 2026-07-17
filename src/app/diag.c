#include "app/diag.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static int g_enabled = 0;
static char g_path[MAX_PATH * 2];

static const char *level_name(diag_level l)
{
    switch (l) {
    case DIAG_ERROR: return "ERROR";
    case DIAG_WARN:  return "WARN ";
    case DIAG_INFO:  return "INFO ";
    default:         return "DEBUG";
    }
}

static void write_raw(const char *s, size_t n)
{
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    WriteFile(g_file, s, (DWORD)n, &w, NULL);
}

int diag_init(int force, const char *invocation)
{
    if (g_enabled)
        return 1;
    if (!force) {
        char buf[8];
        if (GetEnvironmentVariableA("VNC_DEBUG", buf, sizeof(buf)) == 0)
            return 0; /* not requested */
    }

    InitializeCriticalSection(&g_lock);

    wchar_t tmp[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (tn == 0 || tn > MAX_PATH)
        return 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t wpath[MAX_PATH * 2];
    _snwprintf_s(wpath, MAX_PATH * 2, _TRUNCATE,
                 L"%svnc-lightweight-%04u%02u%02u-%02u%02u%02u-%lu.log",
                 tmp, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, (unsigned long)GetCurrentProcessId());

    g_file = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE)
        return 0;
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, g_path, sizeof(g_path), NULL, NULL);
    g_enabled = 1;

    /* Header: build + environment + privacy notice. */
    OSVERSIONINFOEXW osv;
    ZeroMemory(&osv, sizeof(osv));
    osv.dwOSVersionInfoSize = sizeof(osv);
    /* GetVersionExW is deprecated/version-lied without a manifest, but the build
     * number is still informative for triage. */
#pragma warning(push)
#pragma warning(disable : 4996)
    GetVersionExW((LPOSVERSIONINFOW)&osv);
#pragma warning(pop)

    char header[1024];
    int n = _snprintf_s(header, sizeof(header), _TRUNCATE,
        "=== vnc-lightweight-x64 diagnostic log ===\n"
        "This log contains NO passwords, clipboard text, or screen contents.\n"
        "It DOES contain the server host:port (your target) and protocol/error\n"
        "details \xE2\x80\x94 redact the host line before sharing if you prefer.\n"
        "started: %04u-%02u-%02u %02u:%02u:%02u  pid=%lu\n"
        "windows: %lu.%lu build %lu\n"
        "invocation: %s\n"
        "-------------------------------------------\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)osv.dwMajorVersion, (unsigned long)osv.dwMinorVersion,
        (unsigned long)osv.dwBuildNumber,
        invocation ? invocation : "(none)");
    if (n > 0)
        write_raw(header, (size_t)n);
    return 1;
}

int diag_enabled(void) { return g_enabled; }
const char *diag_logpath(void) { return g_enabled ? g_path : ""; }

void diag_log(diag_level level, const char *msg)
{
    if (!g_enabled || !msg)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%02u:%02u:%02u.%03u] [%s] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        level_name(level), msg);
    if (n <= 0)
        return;
    EnterCriticalSection(&g_lock);
    write_raw(line, (size_t)n);
    LeaveCriticalSection(&g_lock);
    OutputDebugStringA(line); /* also visible in a debugger */
}

void diag_logf(diag_level level, const char *fmt, ...)
{
    if (!g_enabled)
        return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0)
        msg[sizeof(msg) - 1] = '\0';
    diag_log(level, msg);
}

void diag_win32(const char *ctx, unsigned long err)
{
    if (!g_enabled)
        return;
    char text[512];
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), text, sizeof(text), NULL);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == '.' || text[len - 1] == ' '))
        text[--len] = '\0';
    if (len == 0)
        strcpy_s(text, sizeof(text), "(no description)");
    diag_logf(DIAG_ERROR, "%s: win32 error %lu (%s)", ctx ? ctx : "?", err, text);
}

void diag_close(void)
{
    if (!g_enabled)
        return;
    diag_log(DIAG_INFO, "log closed");
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_lock);
    DeleteCriticalSection(&g_lock);
    g_enabled = 0;
}
