# Testing

Two layers: **portable core verification** (runs in Linux CI under sanitizers)
and a **Windows manual checklist** (the parts that need real GDI / audio / a
sandbox). The portable protocol core is exercised the same way on both.

## Linux headless verification (the CI gate)

Everything below runs in a plain Linux container. The core protocol code
(vendored libvncclient + `src/core`) is portable C, so decoder correctness and
memory safety are proven here before any Windows build exists.

### The gate itself

The Linux CI job runs two committed scripts, and nothing else:

```
bash tests/ci_linux.sh   # linux-asan: unit tests + 9-encoding cross-process matrix
bash tests/ci_tls.sh     # linux-tls:  VeNCrypt/X509 verification against QEMU
```

Run those to reproduce CI exactly. For build prerequisites, presets and
troubleshooting see **[BUILDING.md](BUILDING.md)** — this file covers only what
the tests prove and how to interpret them.

`ci_linux.sh` drives the matrix through `ipc_test` → the real `vncworker` → the
shared framebuffer, so it exercises the worker and IPC boundary, not just the
decoders. It also runs both unit-test binaries (`audio_test`, `ftpath_test`).

`linux-asan` enables AddressSanitizer + UBSan with `-fno-sanitize-recover`, so
any memory-safety or undefined-behavior finding aborts the run. UBSan's
`alignment`, `shift` and `signed-integer-overflow` checks are scoped OFF for the
vendored sources only (unaligned word loads in the decoders, and signed
shift/overflow in the byte-swap macros — both well-defined on our x86-64
targets). AddressSanitizer stays fully on for the vendored decoders, and our own
code keeps every check.

The sections below describe the same ground manually, for when you are
investigating a failure rather than gating a change.

### Start a test server

Any RFB server works. TigerVNC's `Xvnc` is easiest:

```
printf 'secret\nsecret\n\n' | vncpasswd /tmp/vncpw
Xvnc :9 -geometry 800x600 -depth 24 -SecurityTypes VncAuth -rfbauth /tmp/vncpw -localhost &
# now listening on 127.0.0.1:5909
```

For QEMU (the primary product target, exercises the QEMU extensions):

```
qemu-system-x86_64 -display none -vnc 127.0.0.1:1 -m 256   # -> 127.0.0.1:5901
```

### Per-encoding checksum matrix (decoder regression proof)

`vnctest` prints an FNV-1a checksum of the **visible** framebuffer (RGB only;
the unused 32bpp pad byte is excluded because decoders legitimately differ
there). Every encoding must reproduce the *same* checksum against the same
screen — that is the proof each decoder agrees pixel-for-pixel.

```
export VNC_PASSWORD=secret
for enc in raw copyrect rre corre hextile zlib trle zrle ultra; do
  ./build/linux-asan/vnctest 127.0.0.1:5909 --encodings "$enc" --frames 3 --timeout-ms 7000
done
```

Pass criteria: identical `checksum=` for all encodings, and **zero** sanitizer
output on stderr. VNC Authentication (DES) is exercised by the password login;
a wrong `VNC_PASSWORD` must fail closed with "Authentication failure".

`vnctest` reads the password only from `$VNC_PASSWORD` (never argv). `--ppm FILE`
and `--png FILE` dump a snapshot for eyeballing (both expect the framebuffer in
B,G,R,X order — see `client.c`, which requests that format). `--resize WxH`
exercises the client-driven ExtendedDesktopSize request (the client sends
`SetDesktopSize`; whether the desktop actually changes depends on the server
having a resizable display).

### QEMU audio parser (unit + fuzz + live)

The QEMU audio extension is the one protocol parser we wrote, so it is tested
three ways:

```
# 1. Deterministic unit tests (under the ASan/UBSan preset)
./build/linux-asan/audio_test

# 2. Fuzzing (clang libFuzzer). Requires clang + compiler-rt.
cmake -S . -B build/fuzz -G Ninja -DENABLE_FUZZ=ON -DCMAKE_C_COMPILER=clang
cmake --build build/fuzz --target qemu_audio_fuzz
./build/fuzz/qemu_audio_fuzz tests/fuzz/corpus/audio    # seed corpus committed

# 3. Live negotiation against real QEMU (no guest sound needed to prove the
#    negotiation is accepted and the connection stays healthy):
qemu-system-x86_64 -display none -vnc 127.0.0.1:2 -m 128 \
  -audiodev none,id=a0 -device AC97,audiodev=a0 &
./build/linux-asan/ipc_test 127.0.0.1:5902 --audio --updates 3 --worker ./build/linux-asan/vncworker
# expect: audio_format=1, connection alive, zero ASan output. Actual PCM
# (audio_bytes>0) requires a guest producing sound.
```

### VeNCrypt / X509 TLS (verified in-container via the GnuTLS reference)

The shipped TLS backend is `src/core/tls_schannel.c` (Windows SChannel, no deps).
It cannot run in the Linux CI, so the VeNCrypt/X509 protocol behavior and our
certificate-verification wiring are verified against a REFERENCE build that
compiles the vendored `tls_gnutls.c` with system GnuTLS. tls_schannel.c must
match this behavior (it is otherwise pending Windows-host validation).

```
# Reference TLS build (Linux; needs libgnutls28-dev + pkg-config)
cmake --preset linux-tls
cmake --build build/linux-tls --target vnctest

# Generate a test CA + server cert (certtool from gnutls-bin), then run QEMU
# with x509 VNC:
qemu-system-x86_64 -display none -m 128 \
  -object tls-creds-x509,id=tls0,dir=CERTDIR,endpoint=server,verify-peer=no \
  -vnc 127.0.0.1:3,tls-creds=tls0 &

export ASAN_OPTIONS=detect_leaks=0   # vendored GnuTLS leaks at exit (test-only backend)
./build/linux-tls/vnctest 127.0.0.1:5903 --ca CERTDIR/ca-cert.pem   # -> connects
./build/linux-tls/vnctest 127.0.0.1:5903 --ca CERTDIR/other-ca.pem  # -> "not trusted", rejected
./build/linux-tls/vnctest 127.0.0.1:5903                            # -> no CA, fails closed
```

Expected: correct CA connects over VeNCrypt X509; an unrelated CA is rejected
("The certificate is not trusted"); no CA fails closed. Verified: no
memory-safety findings (only GnuTLS exit leaks, which the SChannel build does not
have). On Windows, repeat against QEMU using `--ca` with a PEM bundle and confirm
the SChannel path rejects a wrong/expired/hostname-mismatched cert.

**Caveat — the GnuTLS reference is more permissive than the product.** The
shipped SChannel backend refuses anonymous/non-X509 VeNCrypt subtypes and
anonymous TLS (security type 18); the vendored GnuTLS reference does NOT (it will
complete anonymous TLS). So two properties are *shipped-backend guarantees*
verified only on Windows, not by the Linux GnuTLS test: (1) rejection of an
anonymous-TLS / non-X509 downgrade when `--ca` is set, and (2) the
`tlsSession`-based cleartext-downgrade guard being exact (on GnuTLS, `tlsSession`
can be set for anon TLS). When testing on Windows with `--ca`, also confirm a
server offering only anonymous TLS (`-object tls-creds-anon`) is rejected, and a
server forced to `RFB 003.003` offering `None`/`VncAuth` is rejected (no cleartext
downgrade, no password sent).

## The Windows binary under test

Build it per **[BUILDING.md](BUILDING.md)** — from an x64 Native Tools Command
Prompt, `cmake --preset win-ninja-release && cmake --build build/win-ninja-release`,
which is the same Ninja/Release path the Windows CI job compiles.

A **single** `vncviewer.exe` lands in the build's output directory (for the Ninja
presets, directly under the build dir; for the version-pinned Visual Studio
presets, under `<binaryDir>/Release/`).
That one executable is the whole product: launched normally it is the trusted UI;
it re-launches *itself* with a hidden `--worker` flag to become the sandboxed
decoder child, and `--headless` runs the console diagnostic client (the Linux
`vnctest` equivalent). The GUI DLLs are delay-loaded so the `--worker` process
never maps `user32`/`gdi32` and keeps its no-win32k confinement.

Launched with **no arguments**, `vncviewer.exe` opens a small dialog asking for
host/port and options (view-only, audio, TLS CA file) instead of printing usage.

## Collecting a debug log (Windows)

If something doesn't work, run with diagnostics on and share the log:

```
vncviewer.exe HOST:PORT --debug        # or set VNC_DEBUG=1
```

A single timestamped log is written to `%TEMP%\vnc-lightweight-*.log` (the exact
path is shown in a message box on exit). It captures the full timeline:

- UI milestones: sandbox spawn steps, worker pid, first frame painted, resize,
  status changes, audio format, cursor, clipboard **lengths**;
- the worker's libvncclient/TLS protocol log (version handshake, security type,
  `TLS handshake done`, certificate trust result, pixel format, disconnect);
- Win32/SSPI error codes with their text (via `diag_win32`), and the worker's
  exit code.

**Privacy:** the log never contains passwords, clipboard text, or screen/pixel
contents — only lengths, dimensions, protocol milestones, and status codes. It
does include the server `host:port` and the server-reported desktop name (needed
to diagnose connection issues); redact those lines before sharing if you prefer.
The header states this in the file itself.

Common first-run signals in the log:
- `sandbox: CreateProcess ... error 1058/5` → the `--worker` re-launch was
  rejected; verify `vncviewer.exe` was built `/guard:cf /CETCOMPAT` (the worker
  is this same image re-launched with `--worker`).
- `status: connect-failed` with no server lines → network/port/TLS before RFB.
- `Server certificate not trusted` → wrong/missing `--ca` bundle.

## Windows manual checklist (per milestone)

Build the product the way CI does: from an **x64 Native Tools Command Prompt**,
`cmake --preset win-ninja-release && cmake --build build/win-ninja-release`
(see [BUILDING.md](BUILDING.md)).

- **M2 shell + sandbox**: connect to a QEMU VM; verify render at 16/32bpp,
  keyboard incl. shifted symbols, mouse + wheel, clean disconnect on close.
  In Process Explorer confirm the `--worker` child (a second `vncviewer.exe`
  instance) runs with an AppContainer SID and the ACG / CIG / no-win32k mitigation
  flags set, and cannot create files or windows.
  - The worker is spawned with **STRICT Control Flow Guard** + **CET** ALWAYS_ON,
    so it must be built `/guard:cf /CETCOMPAT` (CMake's `harden_windows_target`
    does this). If `CreateProcess` fails at spawn, check those flags first.
  - **CIG** (`BLOCK_NON_MICROSOFT_BINARIES`) means every DLL the worker loads
    must be Microsoft-signed. This holds today: zlib/libvncclient are vendored
    and compiled in, and the DLLs the worker actually maps are all MS-signed
    system libraries — `ws2_32`, `secur32`, `crypt32`, plus the MS
    CRT. (The primary EXE image itself is exempt from CIG, so the unsigned
    `vncviewer.exe` runs fine.) The **GUI** DLLs — `user32`, `gdi32`, `shell32`,
    `comdlg32`, `userenv`, `winmm` — plus **`advapi32`** (SID/ACL work is
    UI-side only) are **delay-loaded**, so the worker never
    maps them; that is also what keeps the **no-win32k** filter satisfied (loading
    `user32` would make win32k calls in its DllMain). The mode dispatch parses the
    command line itself (`cmdline_to_wargv`) precisely to avoid `CommandLineToArgvW`,
    which lives in `shell32` and would drag in `user32`. Adding any non-MS DLL
    dependency to the worker path — or making the worker call a delay-loaded GUI
    function — will make it fail to start. Keep worker-reachable dependencies
    static/system-only.
- **M3 encodings + clipboard + cursor**: default encodings negotiate; text
  copy/paste both directions (RFB Extended Clipboard vs QEMU `qemu-vdagent`);
  remote cursor shape tracks the guest.
- **M4 QEMU ext key + LED**: arrows / numpad / left-vs-right modifiers / non-US
  layout correct; LED state reflected.
- **M5 QEMU audio**: audible playback from a guest; pause/resume; no long-run
  drift.
- **M6 VeNCrypt/X509**: TLS + certificate-verified connection to QEMU
  (`-object tls-creds-x509,...`); bad cert is rejected.
- **M7 polish**: default aspect-preserving scaling tracks window resizes;
  `--stretch` fills the client area and `--scale-1to1` disables scaling;
  fullscreen toggles with F11 / Ctrl+Alt+F (and `--fullscreen` at launch); a
  server disconnect raises the reconnect prompt.
- **UX**: "Resize guest to window" drives ExtendedDesktopSize; Save… writes a
  PNG screenshot; the Alt+Space system menu sends Ctrl+Alt+Del / Ctrl+Alt+F1·F2 /
  Ctrl+Esc and toggles fullscreen, cursor lock and disconnect; the title bar
  shows the status line; connection recents persist across launches.
- **M8 file drag-drop (capture only — no upload in this build)**: drop files on
  the viewer. Expect a "File transfer" dialog reporting how many passed
  `ft_sanitize_remote_name`, and stating that the transport is not implemented —
  QEMU has no file channel. Nothing is sent to the guest; this checks the
  path-traversal defense and the drop plumbing, not an upload.
