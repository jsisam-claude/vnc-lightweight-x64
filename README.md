# vnc-lightweight-x64

A minimal, security-hardened **VNC (RFB) client for x64 Windows**. One small C
codebase, no package manager, all third-party code vendored as source and
compiled straight into the executable. Primary target: QEMU's built-in VNC
server, fully featured.

## Status

Under construction, milestone by milestone (see the plan). **M1 is complete**:
the vendored protocol core builds and is proven against real servers.

- [x] **M1** — vendor libvncclient + zlib; portable core; headless proof
- [x] **M2** — two-process split: IPC + sandboxed decoder worker + Win32 shell
      (worker/IPC/core verified cross-process on Linux; Win32 GUI + AppContainer
      pending validation on a Windows host)
- [ ] **M3** — full encoding set + clipboard (RFB Extended Clipboard) + cursor
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

## What it is / isn't

- **Single language (C).** Winsock2 + GDI + waveOut for the Windows product;
  no UI toolkit, no external libraries.
- **No package manager.** No vcpkg/conan/nuget. Third-party sources live under
  `third_party/` and are refreshed per `third_party/UPDATING.md`.
- **Fully RFB-compliant** (RFC 6143): Raw/CopyRect/RRE/CoRRE/Hextile/TRLE/ZRLE/
  Zlib decoders, VNC Authentication, DesktopSize/Cursor pseudo-encodings, plus
  QEMU's extensions.

## Building

Requires only CMake and a C compiler (Visual Studio Enterprise 2022 on Windows).

```
# Windows product (from a VS 2022 developer environment, or "Open Folder" in VS):
cmake --preset vs2022-x64
cmake --build build/vs2022-x64

# Portable headless test client (Linux/macOS/Windows), with sanitizers:
cmake --preset linux-asan
cmake --build build/linux-asan
```

See `docs/TESTING.md` for how to verify against a real server.

## Security

The VNC server is treated as fully untrusted input. Defense is layered —
prevention (latest-release pinning, minimized attack surface, trust-boundary
validation), discovery (permanent ASan/UBSan gate + fuzzing of every parser),
and **containment**: in the Windows product all protocol parsing runs in a
separate `vncworker` process inside an AppContainer with ACG/CIG, the win32k
syscall surface removed, and no filesystem/registry/UI/network reach, brokered
to a thin trusted UI process. A compromised decoder gets a sandbox with nothing
in it. Details live in the project plan and in code comments.

### Transport posture

VeNCrypt / X509 TLS (M6) is the recommended transport: pass `--ca <bundle.pem>`
to verify the server certificate against your CA. The shipped SChannel backend
accepts **X509 VeNCrypt subtypes only** — anonymous TLS (VeNCrypt `TLS*`
subtypes and RFB security type 18) is refused, since it provides no
man-in-the-middle protection — and fails closed if the certificate does not
verify (no trust-on-first-use). Without `--ca`, or against a server offering no
X509 subtype, the TLS handshake is rejected.

Plain RFB (no `--ca`, non-TLS server) uses VNC Authentication — a weak DES
challenge over plaintext. Use it only over localhost, an SSH tunnel, or a
trusted network.

## Licensing

GPL-2.0-or-later (see `LICENSE`). This is a combined work with vendored
libvncclient (GPL-2.0-or-later) and zlib (zlib license); their notices are
retained under `third_party/`. This repository is itself the corresponding
source distribution.
