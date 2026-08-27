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
- `src/common/turbojpeg.c`, `turbojpeg.h` — needed for the Tight encoding *at
  all*, not just its JPEG sub-path: upstream wraps the whole of `tight.c` in
  `#ifdef LIBVNCSERVER_HAVE_LIBZ` / `#ifdef LIBVNCSERVER_HAVE_LIBJPEG` and
  includes `turbojpeg.h` inside that guard, so with LIBJPEG undefined the basic /
  palette / gradient Tight paths compile out too. That is also why `tight.c` can
  sit in the `#include`d-decoder list below while contributing no code. Adding
  Tight would require vendoring libjpeg-turbo — deliberately out of scope, see
  "Deliberately out of scope" in `README.md`.
- `src/common/base64.c/.h` — WebSockets only; we do not build WebSockets.

### What is copied but NOT compiled

Controlled in `CMakeLists.txt` (`LIBVNCCLIENT_TUS`), not by deleting files:

- `src/libvncclient/sasl.c`, `sasl.h` — SASL auth. Not compiled (no Cyrus SASL).
  `sasl.h` is copied because `rfbclient.c` includes it unconditionally.
- `src/common/sha1.c` — SHA1 helper (note: `common/`, not `libvncclient/`). Not
  compiled: its only consumer (`crypto_included.c`'s `hash_sha1`) is entirely
  `#ifdef LIBVNCSERVER_WITH_WEBSOCKETS`, which we never define, so it would only
  add dead object code. `sha.h` stays copied because `crypto_included.c` includes
  it unconditionally; `sha-private.h` is copied only because the uncompiled
  `sha1.c` includes it (nothing we compile references it).
- `src/libvncclient/tls_openssl.c` — OpenSSL TLS backend. Never compiled (we do
  not depend on OpenSSL). `tls.h` is copied because `rfbclient.c` includes it
  unconditionally.
- `src/libvncclient/tls_gnutls.c` — the TLS backend selection is build-specific
  (see `CMakeLists.txt` `TLS_TU`): the Windows **product** compiles our
  `src/core/tls_schannel.c` (SChannel); the Linux **reference / CI** build
  (`VNC_WITH_GNUTLS=ON`, the `linux-tls` preset, exercised by `tests/ci_tls.sh`
  on every push) compiles this vendored `tls_gnutls.c`; everything else compiles
  `tls_none.c`. So `tls_gnutls.c` is a LIVE backend on the CI path and its diff
  MUST be security-reviewed on every refresh — see the known-issues note below.

### The `#include`d decoder trick (important)

`rfbclient.c` `#include`s the decoder `.c` files and `common/vncauth.c` directly
(the multi-bit-depth template trick), and `zrle.c` in turn `#include`s
`common/zywrletemplate.c`. All of them are therefore **present in the tree but
must NOT appear in `LIBVNCCLIENT_TUS`**:
`corre.c hextile.c rre.c tight.c trle.c ultra.c zlib.c zrle.c`,
`common/vncauth.c`, `common/zywrletemplate.c`.
Compiling any of them separately causes duplicate-symbol link errors.

### Notes from the 2026-08-11 refresh (0.9.15 → `42494999`)

- `rfbconfig.h.cmakein` changed its include-guard line (`#cmakedefine … 1` →
  `#define …`); our hand-written `config/rfb/rfbconfig.h` already had a proper
  guard, so no reconciliation was needed.
- `src/common/crypto_included.c` now compiles its SHA1 helper only under
  `LIBVNCSERVER_WITH_WEBSOCKETS`, leaving `sha1.c` unreferenced; it was dropped
  from `LIBVNCCLIENT_TUS` (dead object code otherwise). The file is still copied.
- `rfbclient.c` still does not consume server message 255 (QEMU audio hook
  intact), and the decoder `#include` structure is unchanged.

#### Known upstream issues in this pin (review on the next refresh)

The `42494999` master snapshot carries a few client-side defects. None is a
regression in *our* shipped-and-reachable behavior today, but each is recorded
here so the next refresher re-checks whether upstream fixed it (and can then
drop the corresponding note / local mitigation):

- **`sockets.c` busy-spin (upstream `ad559271`, all builds).** `WaitForMessage`
  now returns 1 immediately when `client->buffered > 0`, but `ReadFromRFBServer`
  reuses `WaitForMessage(client, USECS_WAIT_PER_RETRY)` as its EAGAIN back-off
  sleep. In the small-read branch `buffered` is the *partial-fill* counter, so a
  message body split across TCP segments spins read→EAGAIN→instant-return at
  100% CPU until the rest arrives (worst case: a server that stalls mid-message
  pins a core while the worker holds `api_lock`). The socket is non-blocking
  (`ConnectClientToTcpAddr6WithTimeout` never restores blocking). Contained by
  the sandbox Job object; a malicious server can already freeze the session by
  not sending. If upstream has not fixed it, weigh a recorded patch at the
  `ReadFromRFBServer` retry sites (do NOT edit in place without a patch entry).
- **`tls_gnutls.c` dropped its "no CA ⇒ fail closed" guard (upstream
  `b5dfe0d9`).** `CreateX509CertCredential` now falls back to the OS system
  trust store when no `x509CACertFile` is set, instead of returning NULL. Our
  `cb_get_credential` (src/core/client.c) already returns NULL when no `--ca` is
  configured, and now also fails closed if `strdup(ca_file)` fails, so it never
  hands the backend a credential with a NULL CA path — but any future credential
  path that omits the CA file would silently trust the system store on the CI
  build. Keep that invariant.
- **`tls_gnutls.c` double-free + leak on error paths.** On a
  `gnutls_credentials_set` failure `HandleVeNCryptAuth` frees the callback data
  via `FreeTLS` and then `free()`s it again; an `InitializeTLSSession` failure
  leaks the credential. Rare error paths, GnuTLS reference build only (never
  shipped). `tests/ci_tls.sh` runs with `detect_leaks=0`, so the leak is not
  gated.
- **`ultra.c` `HandleUltraBPP` short-decompress.** Unlike the newly-hardened
  UltraZip path, a decompressed length shorter than the rectangle still calls
  `GotBitmap` for the full rect, copying stale/uninitialized `raw_buffer` tail
  into the framebuffer. `ultra` is in our default encodings; contained by the
  worker sandbox (the worker only ever exposes decoded pixels, which the UI
  caps), but worth watching for an upstream fix.
- **`vncviewer.c` `-repeaterdest` parses `argv[i]` instead of `argv[i+1]`, and
  `parse_host_and_port` no longer NULL-checks its allocations.** Dead for us —
  `vnc_client_connect` calls `rfbInitClient` with `argc=0`/`argv=NULL`, so the
  option parser never runs — but it would become live if any entry point ever
  forwards a real argv.

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

We vendor the **deflate + inflate** core. Inflate is needed for ZRLE and Zlib
decoding (`zrle.c`, `zlib.c`) and for the inbound RFB Extended Clipboard
(`rfbclient.c`) — **not** for TRLE, which is a pure tile/palette decoder with no
zlib use at all. Deflate plus `compressBound()` is needed for the outbound
Extended Clipboard (`rfbclient.c`'s `CompressClipData`, M3), and `compress.c`
additionally for **our own** `src/core/png_write.c`, which calls
`compressBound()`/`compress2()` to write PNG screenshots (M7) and is linked into
every target. We omit the gzip file wrappers (`gz*.c`), `infback.c`, and
`uncompr.c` — none are reachable from libvncclient's client path.

---

## Refresh checklist (do this EVERY time you bump a pin)

1. Re-run the copy commands above with the new ref (a release tag when one
   exists; otherwise the pinned master commit — libvncserver is currently on a
   commit pin, see §1); update the ref/commit/date tables here.
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
5. Re-run **both** CI gates — exactly what `.github/workflows/ci.yml` runs:
   - `bash tests/ci_linux.sh` — `linux-asan` preset; ASan/UBSan unit tests plus
     the 9-encoding cross-process matrix. All encodings must match and be
     sanitizer-clean.
   - `bash tests/ci_tls.sh` — `linux-tls` preset. **This is the only gate that
     compiles the vendored `tls_gnutls.c` at all** (`linux-asan` leaves
     `VNC_WITH_GNUTLS` off and builds `tls_none.c`), so skipping it means the
     TLS backend you were told to security-review in step 3 was never even built.
6. Confirm the compiled translation-unit list in `CMakeLists.txt` still matches
   upstream's client source list (new decoders `#include`d vs separately built).
7. **Walk the "Known upstream issues in this pin" list in §1.** For each entry,
   re-read the corresponding code in the *new* snapshot and either delete the
   note (upstream fixed it — say so in the refresh notes) or carry it forward
   with its status confirmed. Without this step those recorded defects silently
   outlive the pin they describe, and a fix we are waiting on never gets noticed.

## Local patches

None.
