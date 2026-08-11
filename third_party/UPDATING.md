# Updating vendored third-party sources

All third-party code is **vendored as source** and compiled directly into our
targets by the top-level `CMakeLists.txt`. We never run an upstream build system
and never link upstream as a prebuilt library. This file is the single source of
truth for refreshing that code.

**Golden rule:** vendored files are copied *pristine* and are never edited in
place. If a local change is ever unavoidable, add a patch under
`third_party/patches/` and list it in the "Local patches" section below (there
are none today).

---

## 1. libvncserver (provides libvncclient)

| | |
|---|---|
| Upstream | https://github.com/LibVNC/libvncserver |
| Pinned ref | `master` (no release tag yet — see below) |
| Pinned commit | `42494999e6492aaab9c1db785ecd293ef10b3aed` (2026-07-06) |
| Previous pin | `LibVNCServer-0.9.15` / `9b54b1ec32731bd23158ca014dc18014db4194c3` |
| Vendored on | 2026-08-11 |
| License | GPL-2.0-or-later (see `libvncserver/COPYING`) |

**Why a commit pin instead of a release tag:** the latest release (0.9.15) predates
several client-side security fixes that only exist on `master` as of this refresh —
notably the Tight basic-compression row-clamp (heap OOB write, `540332be`, merged
from a security-advisory fork), the Tight gradient-decoding overflow fix
(`5b270544`), and bounds checks in UltraZip subrectangle parsing (`009008e2`,
`ultra.c` — a decoder we compile). Our own policy ("a client-affecting advisory is
itself a refresh trigger") demands these; return to tag-pinning at the next
upstream release that contains them. The clone command below therefore omits
`--branch` and checks out the pinned commit instead.

### What we copy

```
git clone https://github.com/LibVNC/libvncserver.git /tmp/lvns
git -C /tmp/lvns checkout 42494999e6492aaab9c1db785ecd293ef10b3aed

# headers — entire dir (small; keeps refresh a plain copy)
cp /tmp/lvns/include/rfb/*.h            third_party/libvncserver/include/rfb/
cp /tmp/lvns/include/rfb/rfbconfig.h.cmakein \
                                        third_party/libvncserver/include/rfb/

# client sources — entire dir (.c and .h)
cp /tmp/lvns/src/libvncclient/*.c /tmp/lvns/src/libvncclient/*.h \
                                        third_party/libvncserver/src/libvncclient/

# common — curated subset ONLY (attack-surface minimization)
for f in sockets.c sockets.h vncauth.c d3des.c d3des.h minilzo.c minilzo.h \
         lzoconf.h lzodefs.h zywrletemplate.c crypto.h crypto_included.c \
         sha1.c sha.h sha-private.h; do
  cp /tmp/lvns/src/common/$f            third_party/libvncserver/src/common/
done

cp /tmp/lvns/COPYING                    third_party/libvncserver/COPYING
```

### What we deliberately DO NOT copy (and why)

- **All server code** (`src/libvncserver/`, `examples/`) — we are a client only.
- `src/common/crypto_openssl.c`, `crypto_libgcrypt.c` — external crypto backends;
  we use `crypto_included.c` (no external deps). VNC Authentication (DES) works;
  Apple-ARD / RSA-AES paths are compiled but their AES/DH stubs return failure.
- `src/common/turbojpeg.c`, `turbojpeg.h` — only needed for the Tight encoding
  with JPEG. Deferred (see M9); would require vendoring libjpeg-turbo.
- `src/common/base64.c/.h` — WebSockets only; we do not build WebSockets.

### What is copied but NOT compiled

Controlled in `CMakeLists.txt` (`LIBVNCCLIENT_TUS`), not by deleting files:

- `src/libvncclient/sasl.c`, `sasl.h` — SASL auth. Not compiled (no Cyrus SASL).
  `sasl.h` is copied because `rfbclient.c` includes it unconditionally.
- `src/libvncclient/tls_gnutls.c`, `tls_openssl.c` — real TLS backends. Not
  compiled; we build `tls_none.c` now and will add an OS-SChannel backend later
  (see M6). `tls.h` is copied because `rfbclient.c` includes it unconditionally.

### The `#include`d decoder trick (important)

`rfbclient.c` `#include`s the decoder `.c` files and `vncauth.c` directly (the
multi-bit-depth template trick), so they are **present in the tree but must NOT
appear in `LIBVNCCLIENT_TUS`**:
`corre.c hextile.c rre.c tight.c trle.c ultra.c zlib.c zrle.c`,
`common/vncauth.c`, `common/zywrletemplate.c`.
Compiling any of them separately causes duplicate-symbol link errors.

### Notes from the 2026-08-11 refresh (0.9.15 → `42494999`)

- `rfbconfig.h.cmakein` changed its include-guard line (`#cmakedefine … 1` →
  `#define …`); our hand-written `config/rfb/rfbconfig.h` already had a proper
  guard, so no reconciliation was needed.
- `src/common/crypto_included.c` now compiles its SHA1 helper only under
  `LIBVNCSERVER_WITH_WEBSOCKETS`; `sha1.c` is therefore unreferenced in our
  build but stays in the compile list (harmless, keeps the list aligned with
  the copy list).
- `tls_gnutls.c` (Linux reference backend) gained a system-CA fallback and an
  expected-fingerprint API. Our fail-closed posture is unaffected — we always
  supply an explicit CA file in the credential — and `tests/ci_tls.sh`
  (reject-on-wrong-CA, fail-closed-without-CA) plus the RFB 3.3 downgrade
  mock both re-verified green after the refresh.
- `rfbclient.c` still does not consume server message 255 (QEMU audio hook
  intact), and the decoder `#include` structure is unchanged.

### `listen.c`

`vncviewer.c` references `listenForIncomingConnections`, so `listen.c` must be
compiled to satisfy the linker. We are a **connect-only** client and never enable
listen mode, so that code is unreachable at runtime. If a future refresh makes it
cleanly separable, drop it from the build.

---

## 2. zlib

| | |
|---|---|
| Upstream | https://github.com/madler/zlib |
| Pinned tag | `v1.3.2` |
| Pinned commit | `da607da739fa6047df13e66a2af6b8bec7c2a498` |
| Vendored on | 2026-07-16 |
| License | zlib license (permissive; see `zlib/LICENSE`) |

### What we copy

```
git clone --depth 1 --branch v1.3.2 https://github.com/madler/zlib.git /tmp/zlib
for f in adler32.c compress.c crc32.c crc32.h deflate.c deflate.h inffast.c \
         inffast.h inffixed.h inflate.c inflate.h inftrees.c inftrees.h \
         trees.c trees.h zconf.h zlib.h zutil.c zutil.h gzguts.h LICENSE; do
  cp /tmp/zlib/$f third_party/zlib/
done
```

We vendor the **deflate + inflate** core. Inflate is needed for ZRLE / Zlib /
TRLE decoding; deflate + `compress()` / `compressBound()` are needed for the RFB
Extended Clipboard (M3). We omit the gzip file wrappers (`gz*.c`), `infback.c`,
and `uncompr.c` — none are reachable from libvncclient's client path.

---

## Refresh checklist (do this EVERY time you bump a pin)

1. Re-run the copy commands above with the new tag; update the tag/commit/date
   tables here.
2. **Diff `include/rfb/rfbconfig.h.cmakein`** (old vs new) and reconcile any
   added/removed `#cmakedefine` into the hand-written
   `third_party/config/rfb/rfbconfig.h`.
3. **Security review**: read LibVNC's GitHub Security Advisories and the release
   notes since the previous pin. A client-affecting advisory is itself a reason
   to refresh. (LibVNC is fuzzed under OSS-Fuzz; we inherit those fixes by
   tracking releases.)
4. **Grep `rfbclient.c` for `rfbQemuEvent` / server message type 255 handling.**
   Our QEMU-audio extension (added in M5) owns server message 255. If upstream
   starts consuming it, our extension hook breaks — reconcile before shipping.
5. Re-run the full verification matrix under the `linux-asan` preset
   (see `docs/TESTING.md`). All encodings must match and be sanitizer-clean.
6. Confirm the compiled translation-unit list in `CMakeLists.txt` still matches
   upstream's client source list (new decoders `#include`d vs separately built).

## Local patches

None.
