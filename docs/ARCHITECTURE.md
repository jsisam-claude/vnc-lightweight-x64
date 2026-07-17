# Architecture

## One binary, two processes, one trust boundary

The product is a **single `vncviewer.exe`** that runs in three modes selected by
the command line (see `src/app/modes.h`). Launched normally it is the trusted UI;
it re-launches *itself* with a hidden `--worker` flag to become the sandboxed
decoder child (Chromium-style); `--headless` is the console diagnostic client.
The trust boundary is between the two *processes*, not two files.

```
   vncviewer.exe (default)                    vncviewer.exe --worker
        ┌─────────────────────────┐         ┌──────────────────────────────┐
        │      trusted UI          │         │  untrusted decoder           │
        │                          │         │  AppContainer sandbox        │
        │  • window + GDI blit     │  IPC    │  • TCP socket to server      │
        │  • keyboard / mouse      │◄───────►│  • libvncclient (all RFB     │
        │  • clipboard             │ channel │    parsing / decoders)       │
        │  • audio (waveOut)       │         │  • zlib inflate              │
        │  • TLS handshake         │         │                              │
        │  • file I/O broker       │         │  no FS / registry / UI /     │
        │  • password prompt       │         │  extra network               │
        └───────────┬──────────────┘         └──────────────┬───────────────┘
                    │        shared framebuffer (32bpp)      │
                    └────────────────────────────────────────┘
                         worker writes pixels, UI blits
```

The **worker** does every byte of untrusted parsing. It runs in a Windows
AppContainer with:

- one capability only: `internetClient` (outbound TCP to the server);
- process mitigations: ACG (no dynamic executable memory), CIG (Microsoft-signed
  images only), win32k syscalls disabled, heap-terminate-on-corruption, forced +
  high-entropy + bottom-up ASLR, strict handle checks, extension-point disable,
  strict CFG, CET shadow stacks;
- access to exactly two IPC pipe ends and the framebuffer mapping, each DACL'd to
  the worker's AppContainer SID — nothing else.

**One binary does not weaken the boundary.** The RFB-decoder code is *compiled
into* `vncviewer.exe`, but it is only ever *executed* in the `--worker` process:
the UI code path never calls a decoder, and a decoder exploit is confined to the
sandboxed process, which has no filesystem, clipboard, UI, or extra-network reach.
Crucially, the merged image keeps the worker's confinement intact because the GUI
DLLs (`user32`/`gdi32`/`shell32`/`comdlg32`/`userenv`/`winmm`) are **delay-loaded**
— the worker never calls them, so they are never mapped, so loading `user32`
(which would issue win32k syscalls in its DllMain and be killed by the no-win32k
mitigation) never happens. The mode dispatch even parses the command line itself
to avoid `CommandLineToArgvW`, which lives in `shell32` and would pull in `user32`.

## IPC (`src/ipc`)

- **Shared framebuffer** (`shm.c`): a named mapping with a small header
  (magic/width/height/generation) followed by 32bpp pixels. The worker writes
  decoded pixels; the UI reads them for `StretchDIBits`.
- **Message channel** (`channel.c`): a duplex byte stream (two anonymous pipes on
  Windows, a socketpair in tests). Every message is `{u32 type, u32 length}` +
  payload. `channel.c` enforces a per-type length cap in *both* directions, so a
  compromised peer on either side cannot induce an over-read or oversize alloc.
  All message structs are in `protocol.h`.

Both endpoints treat the other as hostile: the UI re-validates every event
before acting, and the worker re-validates every command.

## Portability & testing

`src/core` (the libvncclient wrapper) and `src/ipc` are portable C. This lets the
whole worker + IPC + shared-memory pipeline run and be verified on Linux via
`ipc_test`, which plays the UI role headlessly, spawns the real `vncworker`, and
checksums the shared framebuffer — the result matches the direct `vnctest`
client bit-for-bit. On Linux the worker and headless entry points build as those
two standalone binaries (`vncworker`, `vnctest`) via thin `main()` shims; on
Windows the identical entry points (`vnc_worker_main`, `vnc_headless_main`) are
compiled into the single `vncviewer.exe` and reached through `wWinMain`'s mode
dispatch. Only the AppContainer wrapping and the GDI/audio/TLS glue are
Windows-specific and validated on a Windows host (see `docs/TESTING.md`).
