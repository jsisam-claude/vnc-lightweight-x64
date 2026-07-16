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
  ${CMAKE_SOURCE_DIR}/src/core/qemu_audio.c
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
  set(VENDOR_WARN_FLAGS /W0 -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
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

# Runtime hardening for the Windows binaries. The worker is spawned with STRICT
# Control Flow Guard and CET shadow-stack mitigations ALWAYS_ON, so its image
# MUST be built /guard:cf (and marked /CETCOMPAT) or CreateProcess refuses it.
# Applied to both Windows executables via harden_windows_target().
function(harden_windows_target tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /guard:cf /Qspectre)
    target_link_options(${tgt} PRIVATE
      /guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
  endif()
endfunction()

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
# vncworker: the untrusted decoder process. Contains all RFB-parsing code.
# ---------------------------------------------------------------------------
add_executable(vncworker
  ${VENDOR_SOURCES}
  ${CORE_SOURCES}
  ${IPC_SOURCES}
  ${CMAKE_SOURCE_DIR}/src/worker/worker_main.c
)
target_include_directories(vncworker PRIVATE ${VENDOR_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src)
target_compile_options(vncworker PRIVATE ${OUR_WARN_FLAGS})
target_link_libraries(vncworker PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(vncworker PRIVATE ws2_32)
  target_compile_definitions(vncworker PRIVATE WIN32_LEAN_AND_MEAN)
  harden_windows_target(vncworker) # required for the STRICT-CFG spawn to succeed
elseif(UNIX AND NOT APPLE)
  target_link_libraries(vncworker PRIVATE rt) # shm_open
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
# vncviewer: the trusted UI/broker (Windows product). Deliberately does NOT
# link the vendored RFB decoders — all parsing lives in vncworker. Added with
# the Win32 shell sources below.
# ---------------------------------------------------------------------------
if(WIN32)
  add_executable(vncviewer WIN32
    ${IPC_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/app/main_win32.c
    ${CMAKE_SOURCE_DIR}/src/app/viewer_window.c
    ${CMAKE_SOURCE_DIR}/src/app/input_win32.c
    ${CMAKE_SOURCE_DIR}/src/app/clipboard_win32.c
    ${CMAKE_SOURCE_DIR}/src/app/audio_waveout.c
    ${CMAKE_SOURCE_DIR}/src/app/sandbox_win32.c
  )
  target_include_directories(vncviewer PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(vncviewer PRIVATE ${OUR_WARN_FLAGS})
  target_compile_definitions(vncviewer PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_link_libraries(vncviewer PRIVATE user32 gdi32 ws2_32 shell32 comdlg32 advapi32 userenv winmm)
  harden_windows_target(vncviewer)
endif()
