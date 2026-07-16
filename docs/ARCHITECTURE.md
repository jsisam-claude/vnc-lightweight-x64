# Architecture

## Two processes, one trust boundary

```
        ┌─────────────────────────┐         ┌──────────────────────────────┐
        │  vncviewer (trusted UI)  │         │  vncworker (untrusted)       │
        │                          │         │  AppContainer sandbox        │
        │  • window + GDI blit     │  IPC    │  • TCP socket to server      │
        │  • keyboard / mouse      │◄───────►│  • libvncclient (all RFB     │
        │  • clipboard             │ channel │    parsing / decoders)       │
        │  • audio (waveOut)       │         │  • zlib inflate              │
        │  • TLS handshake (M6)    │         │                              │
        │  • file I/O broker (M8)  │         │  no FS / registry / UI /     │
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

The **UI** never contains RFB-decoder code at all (the `vncviewer` target does
not link the vendored `libvncclient`/`zlib`), so a decoder exploit cannot reach
the code that has filesystem, clipboard, and full-network reach.

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
client bit-for-bit. Only the AppContainer wrapping and the GDI/audio/TLS glue are
Windows-specific and validated on a Windows host (see `docs/TESTING.md`).
