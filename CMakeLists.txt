#
# vnc-lightweight-x64 — a minimal, hardened VNC client for x64 Windows.
#
# One build file. No package manager. All third-party code is vendored under
# third_party/ and compiled straight into our targets (never built as external
# libraries). See third_party/UPDATING.md for the vendoring/refresh procedure.
#
cmake_minimum_required(VERSION 3.21)
project(vnc-lightweight-x64 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Vendored include paths (config dir first so our rfbconfig.h wins).
# ---------------------------------------------------------------------------
set(TP  ${CMAKE_SOURCE_DIR}/third_party)
set(LVC ${TP}/libvncserver)

set(VENDOR_INCLUDE_DIRS
  ${TP}/config                 # our hand-written rfb/rfbconfig.h
  ${LVC}/include               # rfb/*.h
  ${LVC}/src                   # so #include "common/..." resolves
  ${LVC}/src/common
  ${LVC}/src/libvncclient
  ${TP}/zlib
)

# ---------------------------------------------------------------------------
# Vendored sources actually COMPILED (the decoders + vncauth.c are #included
# into rfbclient.c, so they are NOT listed here — see UPDATING.md).
# ---------------------------------------------------------------------------
set(LIBVNCCLIENT_TUS
  ${LVC}/src/libvncclient/rfbclient.c
  ${LVC}/src/libvncclient/cursor.c
  ${LVC}/src/libvncclient/listen.c      # unreachable (we never listen); see UPDATING.md
  ${LVC}/src/libvncclient/sockets.c
  ${LVC}/src/libvncclient/vncviewer.c
  ${LVC}/src/libvncclient/tls_none.c    # TLS stubbed; real TLS = OS SChannel later
  ${LVC}/src/common/sockets.c
  ${LVC}/src/common/crypto_included.c   # no-external-dep crypto (VNC-auth DES works)
  ${LVC}/src/common/sha1.c
  ${LVC}/src/common/d3des.c
  ${LVC}/src/common/minilzo.c
)

set(ZLIB_TUS
  ${TP}/zlib/adler32.c
  ${TP}/zlib/compress.c   # compress()/compressBound() for Extended Clipboard
  ${TP}/zlib/crc32.c
  ${TP}/zlib/deflate.c
  ${TP}/zlib/inffast.c
  ${TP}/zlib/inflate.c
  ${TP}/zlib/inftrees.c
  ${TP}/zlib/trees.c
  ${TP}/zlib/zutil.c
)

set(VENDOR_SOURCES ${LIBVNCCLIENT_TUS} ${ZLIB_TUS})

set(CORE_SOURCES
  ${CMAKE_SOURCE_DIR}/src/core/client.c
)

# ---------------------------------------------------------------------------
# Warning policy: our code is warning-clean; vendored code gets suppressions
# scoped to its files only (never edited in place).
# ---------------------------------------------------------------------------
if(MSVC)
  set(OUR_WARN_FLAGS  /W4 /sdl)
  set(VENDOR_WARN_FLAGS /W0 -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
else()
  set(OUR_WARN_FLAGS  -Wall -Wextra)
  # -w silences upstream warnings. -fno-sanitize=alignment: the vendored ZRLE/
  # Tight decoders do unaligned word loads that are well-defined on x86-64 (our
  # only targets: Linux test + x64 Windows). We keep ALL memory-safety checks
  # (ASan bounds/UAF, other UBSan) on; only the benign alignment check is scoped
  # out, and only for third-party code — our own code keeps alignment checking.
  set(VENDOR_WARN_FLAGS -w -fno-sanitize=alignment)
endif()

set_source_files_properties(${VENDOR_SOURCES} PROPERTIES
  COMPILE_OPTIONS "${VENDOR_WARN_FLAGS}")

# ---------------------------------------------------------------------------
# vnctest: portable headless verification client (both OSes; ASan target here).
# ---------------------------------------------------------------------------
add_executable(vnctest
  ${VENDOR_SOURCES}
  ${CORE_SOURCES}
  ${CMAKE_SOURCE_DIR}/src/headless/headless_main.c
)
target_include_directories(vnctest PRIVATE ${VENDOR_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src)
target_compile_options(vnctest PRIVATE ${OUR_WARN_FLAGS})

if(WIN32)
  target_link_libraries(vnctest PRIVATE ws2_32)
  target_compile_definitions(vnctest PRIVATE WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# vncviewer: the Windows product (added in M2). Guarded so Linux configures.
# ---------------------------------------------------------------------------
if(WIN32)
  # add_executable(vncviewer WIN32 ...)  # M2: Win32 shell + sandboxed worker
endif()
