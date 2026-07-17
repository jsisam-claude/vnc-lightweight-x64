#ifndef _RFB_RFBCONFIG_H
#define _RFB_RFBCONFIG_H 1

/*
 * Hand-written replacement for the CMake-generated rfb/rfbconfig.h.
 *
 * Upstream generates this from include/rfb/rfbconfig.h.cmakein by resolving
 * each #cmakedefine for the detected platform. We do NOT run upstream's build,
 * so we resolve it here for exactly the two environments this project targets:
 *
 *   1. Windows x64, MSVC / clang-cl   (#ifdef _WIN32 branch)
 *   2. Linux x64, clang/gcc           (the headless test build only)
 *
 * Both are little-endian x86-64. Feature policy (see third_party/UPDATING.md):
 *   - zlib: ON  (required for ZRLE / Zlib / TRLE decoders + Extended Clipboard)
 *   - JPEG, PNG, LZO, GnuTLS, OpenSSL, libgcrypt, SASL, websockets: OFF
 *   - threads: OFF (the client core is driven single-threaded)
 *
 * WHEN REFRESHING THE VENDORED SOURCES: diff the new
 * include/rfb/rfbconfig.h.cmakein against the copy in this tree and reconcile
 * any added #cmakedefine here.
 */

/* ---- Package version: pinned to the vendored release (LibVNCServer 0.9.15) ---- */
#define LIBVNCSERVER_PACKAGE_STRING     "LibVNCServer 0.9.15"
#define LIBVNCSERVER_PACKAGE_VERSION    "0.9.15"
#define LIBVNCSERVER_VERSION            "0.9.15"
#define LIBVNCSERVER_VERSION_MAJOR      "0"
#define LIBVNCSERVER_VERSION_MINOR      "9"
#define LIBVNCSERVER_VERSION_PATCHLEVEL "15"

/* ---- Always on for our targets ---- */
#define LIBVNCSERVER_HAVE_LIBZ 1

/* Little-endian only (x86-64). LIBVNCSERVER_WORDS_BIGENDIAN intentionally undefined. */

/* ---- Standard C library functions (present on both MSVC and glibc) ---- */
#define LIBVNCSERVER_HAVE_MEMMOVE 1
#define LIBVNCSERVER_HAVE_MEMSET 1
#define LIBVNCSERVER_HAVE_STRCHR 1
#define LIBVNCSERVER_HAVE_STRCSPN 1
#define LIBVNCSERVER_HAVE_STRERROR 1
#define LIBVNCSERVER_HAVE_STRSTR 1
#define LIBVNCSERVER_HAVE_VPRINTF 1
#define LIBVNCSERVER_HAVE_FCNTL_H 1
#define LIBVNCSERVER_HAVE_SYS_STAT_H 1
#define LIBVNCSERVER_HAVE_SYS_TYPES_H 1

#if defined(_WIN32)

/* ---------------------- Windows x64 (MSVC / clang-cl) ---------------------- */
#define LIBVNCSERVER_HAVE_WS2TCPIP_H 1
/* Use Win32 threading primitives. Required, not optional: with NO thread mode,
 * the vendored threading.h expands MUTEX(x) to nothing, leaving a bare `;` as a
 * struct member in rfbclient.h — which MSVC rejects (C2059) even though Clang
 * tolerates it. CRITICAL_SECTION comes from windows.h (pulled in by winsock2.h).
 * This matches how libvncclient's own Windows CI builds. */
#define LIBVNCSERVER_HAVE_WIN32THREADS 1
/* strdup exists as _strdup; upstream sockets code handles the WIN32 path. */
#define LIBVNCSERVER_HAVE_GETHOSTBYNAME 1
#define LIBVNCSERVER_HAVE_GETHOSTNAME 1
#define LIBVNCSERVER_HAVE_INET_NTOA 1
#define LIBVNCSERVER_HAVE_SOCKET 1
#define LIBVNCSERVER_HAVE_SELECT 1
/* size_t / socklen_t provided by the Windows SDK headers. */
#define HAVE_LIBVNCSERVER_SIZE_T 1
#define HAVE_LIBVNCSERVER_SOCKLEN_T 1
#define HAVE_LIBVNCSERVER_PID_T 1

#else

/* ---------------------- Linux x64 (clang / gcc) — test build --------------- */
#define LIBVNCSERVER_HAVE_DIRENT_H 1
#define LIBVNCSERVER_HAVE_ENDIAN_H 1
#define LIBVNCSERVER_HAVE_GETTIMEOFDAY 1
#define LIBVNCSERVER_HAVE_GETHOSTBYNAME 1
#define LIBVNCSERVER_HAVE_GETHOSTNAME 1
#define LIBVNCSERVER_HAVE_INET_NTOA 1
#define LIBVNCSERVER_HAVE_MKFIFO 1
#define LIBVNCSERVER_HAVE_SELECT 1
#define LIBVNCSERVER_HAVE_SOCKET 1
#define LIBVNCSERVER_HAVE_STRDUP 1
#define LIBVNCSERVER_HAVE_NETINET_IN_H 1
#define LIBVNCSERVER_HAVE_SYS_SOCKET_H 1
#define LIBVNCSERVER_HAVE_SYS_TIME_H 1
#define LIBVNCSERVER_HAVE_SYS_WAIT_H 1
#define LIBVNCSERVER_HAVE_SYS_UIO_H 1
#define LIBVNCSERVER_HAVE_SYS_RESOURCE_H 1
#define LIBVNCSERVER_HAVE_UNISTD_H 1
#define LIBVNCSERVER_HAVE_MMAP 1
#define HAVE_LIBVNCSERVER_SIZE_T 1
#define HAVE_LIBVNCSERVER_SOCKLEN_T 1
#define HAVE_LIBVNCSERVER_PID_T 1

#endif /* _WIN32 */

/* ---- Deliberately NOT defined (feature policy above) ----
 * LIBVNCSERVER_HAVE_LIBJPEG, LIBVNCSERVER_HAVE_LIBPNG, LIBVNCSERVER_HAVE_LZO,
 * LIBVNCSERVER_HAVE_LIBPTHREAD (Linux uses no-op mutexes; single-threaded test),
 * LIBVNCSERVER_HAVE_WIN32THREADS (defined above for _WIN32 only),
 * LIBVNCSERVER_HAVE_GNUTLS, LIBVNCSERVER_HAVE_LIBSSL,
 * LIBVNCSERVER_HAVE_LIBGCRYPT, LIBVNCSERVER_HAVE_SASL,
 * LIBVNCSERVER_WITH_WEBSOCKETS, LIBVNCSERVER_IPv6,
 * LIBVNCSERVER_WORDS_BIGENDIAN, LIBVNCSERVER_ALLOW24BPP.
 */

#endif /* _RFB_RFBCONFIG_H */
