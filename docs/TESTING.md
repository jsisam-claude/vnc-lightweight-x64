# Testing

Two layers: **portable core verification** (runs in Linux CI under sanitizers)
and a **Windows manual checklist** (the parts that need real GDI / audio / a
sandbox). The portable protocol core is exercised the same way on both.

## Linux headless verification (the CI gate)

Everything below runs in a plain Linux container. The core protocol code
(vendored libvncclient + `src/core`) is portable C, so decoder correctness and
memory safety are proven here before any Windows build exists.

### Build (sanitizer gate)

```
cmake --preset linux-asan
cmake --build build/linux-asan
```

`linux-asan` enables AddressSanitizer + UBSan with `-fno-sanitize-recover`, so
any memory-safety or undefined-behavior finding aborts the run. (UBSan's
`alignment` check is scoped OFF for the vendored decoders only — they do
unaligned word loads that are well-defined on our x86-64 targets; all other
checks, and all checks on our own code, stay on.)

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
dumps a snapshot for eyeballing.

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
# Reference TLS build (Linux; needs libgnutls28-dev)
cmake -S . -B build/linux-tls -G Ninja -DVNC_WITH_GNUTLS=ON \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/linux-tls --target vnctest vncworker

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

## Windows manual checklist (per milestone)

Build with Visual Studio Enterprise 2022 (open the folder; pick the
`vs2022-x64` preset) or `cmake --preset vs2022-x64 && cmake --build ...`.

- **M2 shell + sandbox**: connect to a QEMU VM; verify render at 16/32bpp,
  keyboard incl. shifted symbols, mouse + wheel, clean disconnect on close.
  In Process Explorer confirm `vncworker` runs with an AppContainer SID and the
  ACG / CIG / no-win32k mitigation flags set, and cannot create files or windows.
  - The worker is spawned with **STRICT Control Flow Guard** + **CET** ALWAYS_ON,
    so it must be built `/guard:cf /CETCOMPAT` (CMake's `harden_windows_target`
    does this). If `CreateProcess` fails at spawn, check those flags first.
  - **CIG** (`BLOCK_NON_MICROSOFT_BINARIES`) means every DLL the worker loads
    must be Microsoft-signed. This holds today (zlib/libvncclient are vendored
    and compiled in; the only imports are `ws2_32` + the MS CRT). Adding any
    non-MS DLL dependency to the worker — or building vendored code as a DLL —
    will make it fail to start. Keep worker dependencies static/system-only.
- **M3 encodings + clipboard + cursor**: default encodings negotiate; text
  copy/paste both directions (RFB Extended Clipboard vs QEMU `qemu-vdagent`);
  remote cursor shape tracks the guest.
- **M4 QEMU ext key + LED**: arrows / numpad / left-vs-right modifiers / non-US
  layout correct; LED state reflected.
- **M5 QEMU audio**: audible playback from a guest; pause/resume; no long-run
  drift.
- **M6 VeNCrypt/X509**: TLS + certificate-verified connection to QEMU
  (`-object tls-creds-x509,...`); bad cert is rejected.
- **M8 file drag-drop**: drag a file onto the viewer uploads to the guest (with
  a TightVNC/UltraVNC server in the guest); server-supplied paths are sanitized.
