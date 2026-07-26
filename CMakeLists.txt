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
# TLS backend selection.
#   Windows product           -> src/core/tls_schannel.c (OS SChannel, no deps)
#   Linux TEST/REFERENCE build -> vendored tls_gnutls.c + system GnuTLS, enabled
#                                 with -DVNC_WITH_GNUTLS=ON to verify the whole
#                                 VeNCrypt/X509 path end-to-end in CI.
#   Otherwise                 -> tls_none.c (TLS unavailable)
option(VNC_WITH_GNUTLS "Build the Linux reference TLS backend against system GnuTLS" OFF)

if(WIN32)
  set(TLS_TU ${CMAKE_SOURCE_DIR}/src/core/tls_schannel.c)
elseif(VNC_WITH_GNUTLS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GNUTLS REQUIRED gnutls)
  set(TLS_TU ${LVC}/src/libvncclient/tls_gnutls.c)
else()
  set(TLS_TU ${LVC}/src/libvncclient/tls_none.c)
endif()

set(LIBVNCCLIENT_TUS
  ${LVC}/src/libvncclient/rfbclient.c
  ${LVC}/src/libvncclient/cursor.c
  ${LVC}/src/libvncclient/listen.c      # unreachable (we never listen); see UPDATING.md
  ${LVC}/src/libvncclient/sockets.c
  ${LVC}/src/libvncclient/vncviewer.c
  ${TLS_TU}
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
  ${CMAKE_SOURCE_DIR}/src/core/qemu_audio.c
  ${CMAKE_SOURCE_DIR}/src/core/png_write.c
)

# IPC layer (portable): shared framebuffer + validated message channel. Linked
# by the worker and the UI, but NOT by any RFB-parsing code path on the UI side.
set(IPC_SOURCES
  ${CMAKE_SOURCE_DIR}/src/ipc/shm.c
  ${CMAKE_SOURCE_DIR}/src/ipc/channel.c
)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warning policy: our code is warning-clean; vendored code gets suppressions
# scoped to its files only (never edited in place).
# ---------------------------------------------------------------------------
if(MSVC)
  set(OUR_WARN_FLAGS  /W4 /sdl)
  # Vendored code: warnings off. /sdl- is required because /sdl (from the target)
  # otherwise leaks onto these sources and PROMOTES benign warnings (e.g. C4146,
  # unary minus on unsigned, in the crypto/bit code) to errors; source-level
  # options come after target options on the command line, so /sdl- wins.
  set(VENDOR_WARN_FLAGS /W0 /sdl- /wd4146
      -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
else()
  set(OUR_WARN_FLAGS  -Wall -Wextra)
  # -w silences upstream warnings. The vendored code relies on behaviors that are
  # well-defined on our only targets (x86-64: Linux test + x64 Windows) but are
  # technically UB in portable C: unaligned word loads (ZRLE/Tight decoders) and
  # signed shift/overflow in the byte-swap macros (triggered e.g. by advertising
  # a negative pseudo-encoding id through libvncclient's int* extension API).
  # We disable ONLY these arithmetic/portability UBSan checks, and ONLY for
  # third-party code — AddressSanitizer (the memory-safety checker that matters
  # for untrusted input) stays fully on for the decoders, and our own code keeps
  # every check.
  set(VENDOR_WARN_FLAGS -w
      -fno-sanitize=alignment,shift,signed-integer-overflow)
endif()

set_source_files_properties(${VENDOR_SOURCES} PROPERTIES
  COMPILE_OPTIONS "${VENDOR_WARN_FLAGS}")

# tls_schannel.c is OUR hand-written, security-critical code (it parses the
# untrusted VeNCrypt/TLS handshake). It only rides in VENDOR_SOURCES because it
# plugs into libvncclient's TLS seam (TLS_TU) — it must NOT inherit the vendored
# warning/SDL suppressions. Override it back to our strict flags so /W4 /sdl
# scrutinize it like the rest of src/. (Windows-only: elsewhere TLS_TU is the
# genuinely-vendored gnutls/none backend.)
if(WIN32)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/src/core/tls_schannel.c
    PROPERTIES COMPILE_OPTIONS "${OUR_WARN_FLAGS}")
endif()

# Runtime hardening for the Windows binaries. The worker is spawned with STRICT
# Control Flow Guard and CET shadow-stack mitigations ALWAYS_ON, so its image
# MUST be built /guard:cf (and marked /CETCOMPAT) or CreateProcess refuses it.
# Applied to the single Windows executable (vncviewer) via harden_windows_target().
# /Qspectre needs the optional "MSVC Spectre-mitigated libraries" VS component
# (else MSB8040). Opt-in so the build works out of the box; enable for hardened
# release builds after installing that component:  -DVNC_SPECTRE=ON
option(VNC_SPECTRE "Add /Qspectre (requires the MSVC Spectre-mitigated libs)" OFF)
function(harden_windows_target tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /guard:cf)
    if(VNC_SPECTRE)
      target_compile_options(${tgt} PRIVATE /Qspectre)
    endif()
    target_link_options(${tgt} PRIVATE
      /guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
  endif()
endfunction()

# Attach the selected TLS backend's compile definitions + libraries to a target
# that links libvncclient. LIBVNCSERVER_HAVE_GNUTLS gates ONLY the VeNCrypt/TLS
# security-type SELECTION in rfbclient.c (verified: single use), so we define it
# for the SChannel build too — the name is a libvncclient misnomer; our
# tls_schannel.c provides the actual backend functions.
function(attach_tls tgt)
  if(WIN32)
    target_compile_definitions(${tgt} PRIVATE LIBVNCSERVER_HAVE_GNUTLS)
    target_link_libraries(${tgt} PRIVATE secur32 crypt32)
  elseif(VNC_WITH_GNUTLS)
    target_compile_definitions(${tgt} PRIVATE LIBVNCSERVER_HAVE_GNUTLS)
    target_include_directories(${tgt} PRIVATE ${GNUTLS_INCLUDE_DIRS})
    target_link_libraries(${tgt} PRIVATE ${GNUTLS_LIBRARIES})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# SINGLE-EXECUTABLE MODEL
#   On Windows the product ships as ONE vncviewer.exe with three modes selected
#   by the command line (default UI / --worker sandboxed decoder / --headless
#   diagnostic). The worker and headless entry points (vnc_worker_main /
#   vnc_headless_main) are compiled INTO vncviewer; the GUI DLLs are delay-loaded
#   so the --worker process never loads user32/gdi32 and keeps the no-win32k
#   confinement (see src/app/modes.h, sandbox_win32.c).
#
#   On Linux/CI the SAME entry points build as the standalone `vnctest` and
#   `vncworker` executables (via thin main() shims guarded by the *_STANDALONE
#   defines), which the headless matrix and ipc_test harness spawn.
# ---------------------------------------------------------------------------

# vnctest / vncworker: standalone binaries for Linux verification only. On
# Windows their code lives inside vncviewer.exe as --headless / --worker.
if(NOT WIN32)
  add_executable(vnctest
    ${VENDOR_SOURCES}
    ${CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/headless/headless_main.c
  )
  target_include_directories(vnctest PRIVATE ${VENDOR_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(vnctest PRIVATE ${OUR_WARN_FLAGS})
  target_compile_definitions(vnctest PRIVATE VNC_HEADLESS_STANDALONE)
  attach_tls(vnctest)

  add_executable(vncworker
    ${VENDOR_SOURCES}
    ${CORE_SOURCES}
    ${IPC_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/worker/worker_main.c
  )
  target_include_directories(vncworker PRIVATE ${VENDOR_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(vncworker PRIVATE ${OUR_WARN_FLAGS})
  target_compile_definitions(vncworker PRIVATE VNC_WORKER_STANDALONE)
  target_link_libraries(vncworker PRIVATE Threads::Threads)
  attach_tls(vncworker)
  if(UNIX AND NOT APPLE)
    target_link_libraries(vncworker PRIVATE rt) # shm_open
  endif()
endif()

# ---------------------------------------------------------------------------
# ipc_test: Linux-only cross-process verification of worker + IPC + shm + core.
# Spawns vncworker; contains NO RFB-parsing code itself.
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
  add_executable(ipc_test
    ${IPC_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/test/ipc_test.c
  )
  target_include_directories(ipc_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(ipc_test PRIVATE ${OUR_WARN_FLAGS})
  target_link_libraries(ipc_test PRIVATE Threads::Threads rt)
  add_dependencies(ipc_test vncworker)

  # Deterministic unit test for the QEMU audio parser (the one protocol parser
  # we wrote ourselves). Runs under the ASan/UBSan preset.
  add_executable(audio_test
    ${CMAKE_SOURCE_DIR}/src/core/qemu_audio.c
    ${CMAKE_SOURCE_DIR}/src/test/audio_test.c
  )
  target_include_directories(audio_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(audio_test PRIVATE ${OUR_WARN_FLAGS})

  # Path-traversal defense for file transfer (security-critical, portable).
  add_executable(ftpath_test
    ${CMAKE_SOURCE_DIR}/src/core/ftpath.c
    ${CMAKE_SOURCE_DIR}/src/test/ftpath_test.c
  )
  target_include_directories(ftpath_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(ftpath_test PRIVATE ${OUR_WARN_FLAGS})
endif()

# libFuzzer harness over the audio parser (clang only; -DENABLE_FUZZ=ON).
if(ENABLE_FUZZ)
  add_executable(qemu_audio_fuzz
    ${CMAKE_SOURCE_DIR}/src/core/qemu_audio.c
    ${CMAKE_SOURCE_DIR}/src/test/qemu_audio_fuzz.c
  )
  target_include_directories(qemu_audio_fuzz PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(qemu_audio_fuzz PRIVATE -g -fsanitize=fuzzer,address,undefined)
  target_link_options(qemu_audio_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# ---------------------------------------------------------------------------
# vncviewer: the Windows product — the SINGLE executable. It contains both the
# trusted UI/broker AND (compiled in, but only ever executed in the sandboxed
# --worker child) the vendored RFB decoders + worker loop, plus the --headless
# diagnostic client. wWinMain dispatches on the command line.
#
# The GUI DLLs (user32/gdi32/comdlg32/shell32/winmm/userenv) are DELAY-LOADED:
# the --worker process never calls into them, so they are never mapped and the
# win32k-syscall-disable / ACG / CIG mitigations applied to the worker hold.
# (The mode dispatch itself avoids shell32 — see cmdline_to_wargv.)
# ---------------------------------------------------------------------------
if(WIN32)
  add_executable(vncviewer WIN32
    ${VENDOR_SOURCES}
    ${CORE_SOURCES}
    ${IPC_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/core/ftpath.c
    ${CMAKE_SOURCE_DIR}/src/app/main_win32.c
    ${CMAKE_SOURCE_DIR}/src/app/viewer_window.c
    ${CMAKE_SOURCE_DIR}/src/app/input_win32.c
    ${CMAKE_SOURCE_DIR}/src/app/clipboard_win32.c
    ${CMAKE_SOURCE_DIR}/src/app/audio_waveout.c
    ${CMAKE_SOURCE_DIR}/src/app/sandbox_win32.c
    ${CMAKE_SOURCE_DIR}/src/app/diag.c
    ${CMAKE_SOURCE_DIR}/src/worker/worker_main.c   # --worker mode (in-process)
    ${CMAKE_SOURCE_DIR}/src/headless/headless_main.c # --headless mode
  )
  target_include_directories(vncviewer PRIVATE ${VENDOR_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(vncviewer PRIVATE ${OUR_WARN_FLAGS})
  target_compile_definitions(vncviewer PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  # Always-loaded imports, all win32k-free and Microsoft-signed (CIG-safe) so the
  # sandboxed worker can map them: winsock + the SChannel TLS DLLs. Everything the
  # worker never calls — the GUI DLLs AND advapi32 (SIDs/ACLs are UI-only) — is
  # delay-loaded, so the worker's mapped-DLL set stays minimal and no user32 can
  # be pulled in to violate the no-win32k mitigation.
  target_link_libraries(vncviewer PRIVATE
    ws2_32 secur32 crypt32 Threads::Threads
    user32 gdi32 shell32 comdlg32 userenv winmm advapi32 delayimp)
  target_link_options(vncviewer PRIVATE
    /DELAYLOAD:user32.dll /DELAYLOAD:gdi32.dll /DELAYLOAD:shell32.dll
    /DELAYLOAD:comdlg32.dll /DELAYLOAD:userenv.dll /DELAYLOAD:winmm.dll
    /DELAYLOAD:advapi32.dll)
  attach_tls(vncviewer)               # SChannel VeNCrypt/X509 backend + defs
  harden_windows_target(vncviewer)    # /guard:cf /CETCOMPAT so the CFG/CET spawn works
endif()
