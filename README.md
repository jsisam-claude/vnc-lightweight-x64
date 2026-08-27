# vnc-lightweight-x64

A minimal, security-hardened **VNC (RFB) client for x64 Windows**. One small C
codebase, no package manager, all third-party code vendored as source and
compiled straight into the executable. Primary target: QEMU's built-in VNC
server, fully featured.

## Status

Feature-complete through M7, with M8's transport deliberately deferred (see
below). The portable protocol core, the IPC/worker split, and the TLS path are
proven in Linux CI on every push; the Windows-only code (GUI, AppContainer
sandbox, waveOut, SChannel) is compile-checked by CI but still **pending
validation on a Windows host** — nothing below marked "pending" has been run
against a real desktop.

- [x] **M1** — vendor libvncclient + zlib; portable core; headless proof
- [x] **M2** — two-process split: IPC + sandboxed decoder worker + Win32 shell
      (worker/IPC/core verified cross-process on Linux; Win32 GUI + AppContainer
      pending validation on a Windows host)
- [x] **M3** — full encoding set (`copyrect zrle hextile zlib corre rre trle
      ultra raw`, all nine checksum-verified cross-process in CI) + clipboard
      (RFB Extended Clipboard) + client-side cursor (both Windows-UI halves
      pending Windows-host validation)
- [x] **M4** — QEMU Extended Key Event + LED state
- [x] **M5** — QEMU Audio (parser fuzzed + live-negotiated vs QEMU; waveOut sink
      pending Windows-host validation)
- [x] **M6** — VeNCrypt / X509 TLS (best QEMU-supported auth, via OS SChannel).
      Path + certificate verification verified in-container against QEMU via the
      GnuTLS reference backend; the SChannel product backend is reviewed and
      pending Windows-host validation.
- [x] **M7** — polish: aspect-preserving scaling (+ `--stretch` / `--scale-1to1`),
      fullscreen toggle (F11 / Ctrl+Alt+F, `--fullscreen`), reconnect-on-disconnect
      prompt (Windows UI; pending Windows-host validation)
- [~] **M8** — file drag-drop: path-traversal defense (tested) + drag-drop
      capture done. The upload *transport* (TightVNC file-transfer extension)
      is server-gated — it needs a TightVNC/UltraVNC server in the guest; QEMU's
      VNC has no file channel — and remains to be implemented against such a
      server. See "File transfer" below.
- **CI** — GitHub Actions: Linux ASan encoding matrix + TLS verification + audio
      fuzz, and a Windows MSVC compile of the full Win32/SChannel/sandbox code.
- **UX** — client-driven guest resize (ExtendedDesktopSize; "Resize guest to
      window"), PNG screenshot (Save…), a System-menu action set (Alt+Space:
      Send Ctrl+Alt+Del / Ctrl+Alt+F1·F2 / Ctrl+Esc, fullscreen, cursor lock,
      disconnect), a title-bar status line, and saved connection recents. All
      Windows-UI; pending Windows-host validation.

### Deliberately out of scope (for now)

- **Tight encoding** — omitted on purpose to honor the minimal-dependencies
  goal: libvncclient's Tight decoder is fully gated on libjpeg and uses its
  `JCS_EXT` color spaces, so enabling it means vendoring ~40 files of
  libjpeg-turbo. ZRLE / Hextile / Zlib already give QEMU good (lossless)
  compression, so the dependency isn't worth it here. To add Tight later: vendor
  libjpeg-turbo + the `turbojpeg.c` shim, define `LIBVNCSERVER_HAVE_LIBJPEG`,
  and add `tight.c` + `turbojpeg.c` to the build.
- **TightVNC file-transfer transport** — server-gated (needs a TightVNC/UltraVNC
  guest server; QEMU has no file channel). The path-traversal defense is already
  in place (`core/ftpath.c`).

## What it is / isn't

- **Single language (C).** Winsock2 + GDI + waveOut for the Windows product;
  no UI toolkit, no external libraries.
- **No package manager.** No vcpkg/conan/nuget. Third-party sources live under
  `third_party/` and are refreshed per `third_party/UPDATING.md`.
- **Fully RFB-compliant** (RFC 6143): Raw/CopyRect/RRE/CoRRE/Hextile/TRLE/ZRLE/
  Zlib decoders, VNC Authentication, DesktopSize/Cursor pseudo-encodings, plus
  QEMU's extensions.

## Building

Requires CMake 3.25+, Ninja, and a C compiler — any recent MSVC on Windows, no
particular Visual Studio edition or version. No package manager, no fetch step:
all third-party code is vendored and compiled straight in.

```
# Windows product, from an x64 Native Tools Command Prompt:
cmake --preset win-ninja-release
cmake --build build/win-ninja-release

# Portable headless test client (Linux), with sanitizers:
cmake --preset linux-asan
cmake --build build/linux-asan
```

**[docs/BUILDING.md](docs/BUILDING.md) is the single build guide** — every
preset and target, the exact dependency lists, the Windows toolchain setup, and
troubleshooting. `docs/TESTING.md` covers how to verify against a real server.

## Security

The VNC server is treated as fully untrusted input. Defense is layered —
prevention (pinned, security-reviewed upstream sources, minimized attack
surface, trust-boundary validation), discovery (a permanent ASan/UBSan gate over
the cross-process encoding matrix, plus a libFuzzer harness over the QEMU audio
parser — the one RFB-level parser we wrote ourselves; the vendored decoders are
fuzzed upstream under OSS-Fuzz, whose fixes we inherit by tracking releases),
and **containment**: in the Windows product all protocol parsing runs in a
separate worker process inside an AppContainer with ACG/CIG and the win32k
syscall surface removed. That worker holds exactly one capability —
`internetClient`, because it owns the outbound TCP connection to the server — and
gets no registry or UI reach, no filesystem reach beyond a read-only ACE on the
`--ca` bundle when one is supplied, and nothing else but its two IPC pipe ends
and the framebuffer mapping, each DACL'd to its package SID. Everything else is
brokered to a thin trusted UI process, and a Job object caps what a compromised
worker can *consume* (512 MB commit, no child processes, kill-on-close).

Vendored upstream is pinned and re-reviewed on every refresh per
`third_party/UPDATING.md`: zlib at release `v1.3.2`, and libvncserver at a
post-0.9.15 `master` commit rather than a release tag, because several
client-side security fixes we require are not in any release yet.

The product ships as a **single `vncviewer.exe`** (Chromium-style): launched
normally it is the trusted UI; it re-launches *itself* with a hidden `--worker`
flag to become the sandboxed decoder child. The GUI DLLs are delay-loaded, so the
worker never maps `user32`/`gdi32` and the no-win32k confinement holds even though
one binary contains both roles. Run `vncviewer.exe` with no arguments to get a
connection dialog; `--headless` runs the console diagnostic client. Details live
in the project plan and in code comments.

### Transport posture

VeNCrypt / X509 TLS (M6) is the recommended transport: pass `--ca <bundle.pem>`
to verify the server certificate against your CA. The shipped SChannel backend
accepts **X509 VeNCrypt subtypes only** — anonymous TLS (VeNCrypt `TLS*`
subtypes and RFB security type 18) is refused, since it provides no
man-in-the-middle protection — and fails closed if the certificate does not
verify (no trust-on-first-use). Without `--ca`, or against a server offering no
X509 subtype, the TLS handshake is rejected. Setting `--ca` additionally pins the
security type to VeNCrypt **and** refuses to proceed (including sending any VNC
password) if the negotiated transport turns out not to be TLS — so a server or
MITM that tries an RFB-version/security downgrade to cleartext is rejected rather
than silently accepted.

Plain RFB (no `--ca`, non-TLS server) uses VNC Authentication — a weak DES
challenge over plaintext. Use it only over localhost, an SSH tunnel, or a
trusted network.

### Residual risk

The sandbox contains a compromised decoder so it cannot reach the filesystem, UI,
or network — but within the RFB protocol it can still *lie*: the framebuffer,
cursor, audio, and **clipboard** it reports are attacker-controlled. The UI caps
and bounds-checks all of these, but clipboard text set by the server is applied
to the local clipboard (this is the standard VNC clipboard-sync feature); treat a
connection to an untrusted server as able to change what you later paste. Audio
and the password prompt are gated on explicit opt-in / handshake state so a
compromised worker cannot force playback or phish a mid-session password.

## Licensing

GPL-2.0-or-later (see `LICENSE`). This is a combined work with vendored
libvncclient (GPL-2.0-or-later) and zlib (zlib license); their notices are
retained under `third_party/`. This repository is itself the corresponding
source distribution.
