# Building

The single source of truth for building this project. Every command here is
either what CI runs or is derived from it; anything not exercised by CI is
labelled as such.

There is **no package manager and no fetch step** — all third-party code is
vendored under `third_party/` and compiled straight into the targets. Cloning
the repo is all the dependency setup the C code needs.

## Prerequisites

| | |
|---|---|
| **CMake** | **3.25 or newer.** (`CMakeLists.txt` declares 3.21 and `CMakePresets.json` says the same, but the presets file is schema `version: 6`, which CMake only understands from 3.25 — so 3.25 is the real floor for anyone using `--preset`.) |
| **Ninja** | Required by every preset recommended below. The version-pinned Visual Studio presets are the only ones that don't need it. |
| **C compiler** | C11. Any recent MSVC on Windows — no specific Visual Studio edition or version. GCC or Clang on Linux. |

The project is C only (`project(vnc-lightweight-x64 C)`); there is no C++.

### Linux packages

This is the exact list CI installs, and what each is actually for:

```
sudo apt-get install -y --no-install-recommends \
  cmake ninja-build clang \
  libgnutls28-dev gnutls-bin \
  tigervnc-standalone-server tigervnc-common tigervnc-tools \
  qemu-system-x86
```

- `cmake`, `ninja-build` — the generator every Linux preset uses.
- `libgnutls28-dev` — satisfies `pkg_check_modules(GNUTLS REQUIRED gnutls)`; only
  needed for the `linux-tls` reference build.
- `gnutls-bin` — provides `certtool`, which `tests/ci_tls.sh` uses to mint the
  test CA and server certificates.
- `tigervnc-standalone-server` — provides `Xvnc`, the server the encoding matrix
  runs against.
- `tigervnc-tools` — provides `vncpasswd`. **Easy to miss:** it moved out of
  `tigervnc-common`, and `--no-install-recommends` will not pull it in. Its
  absence is one of the two drifts that turned CI red for 21 consecutive runs.
- `qemu-system-x86` — provides `qemu-system-x86_64` for the TLS gate.

Also required but *not* in the list above: **`pkg-config`**, because
`VNC_WITH_GNUTLS=ON` triggers `find_package(PkgConfig REQUIRED)`. CI gets away
without naming it because GitHub's `ubuntu-latest` image preinstalls it; on a
bare container you must install it yourself or the `linux-tls` preset fails to
configure.

Note that although CI installs `clang`, no Linux preset pins a compiler — the
gate actually compiles with the system default `cc` (GCC on Ubuntu). Only the
fuzz build passes `-DCMAKE_C_COMPILER=clang` explicitly, because
`-fsanitize=fuzzer` is Clang-only.

### Windows toolchain

You need the **`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`** component
(the "MSVC v14x — VS C++ x64/x86 build tools" part of the *Desktop development
with C++* workload). Any edition works, including Build Tools — no full Visual
Studio install is required.

Build from an **x64 Native Tools Command Prompt**, or run `vcvarsall.bat x64`
first. `ninja.exe` comes from the VS "C++ CMake tools for Windows" component.

CI does not hardcode a VS version. It locates whatever is installed:

```cmd
for /f "usebackq tokens=*" %i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" ^
    -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do call "%i\VC\Auxiliary\Build\vcvarsall.bat" x64
```

`vswhere.exe` lives at that fixed path regardless of VS version, and
`-latest -products *` deliberately accepts any edition or version. This is what
makes the build survive Visual Studio major-version drift.

## Quick start

Pick the row for what you want to do. The **CI-proven** column says whether that
exact command runs on every push — if it doesn't, treat it as best-effort.

| Goal | Commands | CI-proven |
|---|---|---|
| **Build the Windows product** | `cmake --preset win-ninja-release`<br>`cmake --build build/win-ninja-release` | ✅ yes |
| **Debug build on Windows** | `cmake --preset windows`<br>`cmake --build build/windows` | no (same generator as above, so it can't hit the VS-pin trap) |
| **Run the full Linux gate** | `bash tests/ci_linux.sh` | ✅ yes |
| **Run the TLS gate** | `bash tests/ci_tls.sh` | ✅ yes |
| **Fast Linux compile loop** | `cmake --preset linux-test`<br>`cmake --build build/linux-test` | no (no sanitizers, **no TLS**) |
| **Fuzz the audio parser** | see [Fuzzing](#fuzzing) — no preset exists | ✅ yes |

On Windows the whole product is the single `build/win-ninja-release/vncviewer.exe`.

## Presets

All eight, with what they actually do. Binaries land directly in `binaryDir` for
the Ninja presets; the Visual Studio presets are multi-config, so their output
nests under `<binaryDir>/Release/` and they need `--config Release` at build time.

| Preset | Generator | Build type | Notes |
|---|---|---|---|
| `linux-asan` | Ninja | Debug | **The verification gate.** ASan + UBSan with `-fno-sanitize-recover=all`. TLS is *not* built (`VNC_WITH_GNUTLS` stays off ⇒ `tls_none.c`). Driven by `tests/ci_linux.sh`. ✅ CI |
| `linux-tls` | Ninja | Debug | Same sanitizers **plus** `VNC_WITH_GNUTLS=ON`, so the vendored `tls_gnutls.c` reference backend is compiled. Driven by `tests/ci_tls.sh`. ✅ CI |
| `win-ninja-release` | Ninja | Release | **The only Windows preset CI builds.** Version-agnostic. ✅ CI |
| `windows` | Ninja | Debug | Version-agnostic Windows debug build. Not run by CI. |
| `linux-test` | Ninja | Debug | Plain build, no sanitizers, no TLS. Not run by CI, and has no `condition`, so CMake will offer it on Windows too even though it isn't meant for it. |
| `win-asan` | Ninja | Debug | Windows AddressSanitizer build (`/fsanitize=address`). Overrides the Debug flags to drop `/RTC1`, which MSVC refuses to combine with ASan. Not run by CI — it is the one Windows sanitizer path and is unvalidated on a Windows host. |
| `vs2022-x64` | **VS 17 2022** | multi-config | ⚠️ Configures **only** if Visual Studio 2022 exactly is installed. |
| `vs2026-x64` | **VS 18 2026** | multi-config | ⚠️ Same, for VS 2026 — and needs a CMake new enough to know that generator. |

### ⚠️ The version-pinned generator trap

`vs2022-x64` and `vs2026-x64` hardcode a Visual Studio generator string. If that
exact VS version is not installed, **CMake fails at configure time**:

```
CMake Error at CMakeLists.txt:9 (project):
  Generator
    Visual Studio 17 2022
  could not find any instance of Visual Studio.
```

This is not hypothetical: when GitHub's `windows-latest` image dropped VS 2022,
this exact error broke every CI run until the workflow was moved to
`win-ninja-release`. Note it is a *configure*-time failure — not the MSBuild
`MSB8020` toolset error, which is a different problem.

Use these presets only if you specifically want the Visual Studio generator (for
"Open Folder" in the IDE) *and* have that version installed. Otherwise prefer the
Ninja presets, which adapt to whatever MSVC is present.

Each configure preset has a build preset of the same name, so
`cmake --build --preset <name>` also works; it is equivalent to
`cmake --build <binaryDir>`.

## Targets

Which targets exist depends on the platform — the guards in `CMakeLists.txt`
mean you never get all of them at once.

| Target | Platform | What it is |
|---|---|---|
| `vncviewer` | Windows only | **The product.** One executable containing all three modes — trusted UI (default), `--worker` (sandboxed decoder), `--headless` (console diagnostic). The only target built on Windows. |
| `vnctest` | not Windows | Standalone headless diagnostic client — the Linux equivalent of `vncviewer.exe --headless`. |
| `vncworker` | not Windows | Standalone sandboxed-decoder binary — the Linux equivalent of `vncviewer.exe --worker`. Spawned by `ipc_test`, not run by hand. |
| `ipc_test` | Linux only | Cross-process verification of worker + IPC + shared memory. Contains no RFB-parsing code itself; spawns the real `vncworker`. |
| `audio_test` | Linux only | Unit test for the QEMU audio parser. |
| `ftpath_test` | Linux only | Unit test for the file-transfer path-traversal defense. |
| `qemu_audio_fuzz` | any, `ENABLE_FUZZ=ON` | libFuzzer harness over the audio parser. Clang only. |

"Linux only" above means `UNIX AND NOT APPLE` — on macOS you get `vnctest` and
`vncworker` but none of the test binaries.

There is **no CTest integration** (no `enable_testing()`, no `add_test()`). The
gates are the two shell scripts, which invoke the binaries directly.

## Options

| Option | Default | Effect |
|---|---|---|
| `VNC_WITH_GNUTLS` | `OFF` | Non-Windows only: build the vendored `tls_gnutls.c` reference TLS backend against system GnuTLS (requires pkg-config + `libgnutls28-dev`). `OFF` selects `tls_none.c` — **no TLS at all**. Ignored on Windows, which always uses our SChannel backend. Set by the `linux-tls` preset. |
| `VNC_SPECTRE` | `OFF` | Windows only: add `/Qspectre`. Needs the optional "MSVC Spectre-mitigated libraries" VS component, or the build fails with `MSB8040`. Opt-in so the build works out of the box; turn it on for hardened release builds. |
| `ENABLE_FUZZ` | unset | Builds `qemu_audio_fuzz`. Note this is **not** a declared `option()`, so it never shows up in `cmake-gui`/`ccmake` — it only works when passed on the command line. |

A bare `cmake -S . -B build` with no preset produces a **Debug** build.

## Running the gates

The two committed scripts are exactly what the Linux CI job runs — there is no
separate CI-only invocation to reproduce.

```
bash tests/ci_linux.sh   # linux-asan: unit tests + 9-encoding cross-process matrix
bash tests/ci_tls.sh     # linux-tls:  VeNCrypt/X509 verification against QEMU
```

`ci_linux.sh` builds the `linux-asan` preset, runs `audio_test` and
`ftpath_test`, starts `Xvnc` on `:19`, then drives all nine encodings
(`raw copyrect rre corre hextile zlib trle zrle ultra`) through
`ipc_test` → `vncworker` → shared framebuffer, and fails if any encoding's
checksum differs from the first or if ASan/UBSan/LSan says anything.

`ci_tls.sh` builds only the `vnctest` target from `linux-tls`, mints a CA plus
server certificate with `certtool`, starts QEMU with `tls-creds-x509`, and
asserts three things: the correct CA connects, an unrelated CA is rejected, and
no CA at all fails closed. It sets `ASAN_OPTIONS=detect_leaks=0` because the
vendored GnuTLS backend leaks at exit — leak detection stays on in `ci_linux.sh`.

See [TESTING.md](TESTING.md) for what these prove and for the Windows manual
checklist.

## Fuzzing

There is no preset for this; the proven invocation is the literal one from CI:

```
cmake -S . -B build/fuzz -G Ninja -DENABLE_FUZZ=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build/fuzz --target qemu_audio_fuzz
./build/fuzz/qemu_audio_fuzz -max_total_time=60 tests/fuzz/corpus/audio
```

Clang is mandatory — `-fsanitize=fuzzer` is not a GCC feature. On Ubuntu the
runtimes come from `libclang-rt-dev`.

## Troubleshooting

**`could not find any instance of Visual Studio.`** — you used a version-pinned
preset without that VS version. Use `win-ninja-release` instead.

**`Could not find toolchain file` / `cl.exe` not found on Windows** — you are not
in a developer command prompt. Run `vcvarsall.bat x64`, or use the vswhere
snippet above.

**`ninja: command not found`** — install `ninja-build` (Linux) or the "C++ CMake
tools for Windows" VS component.

**`vncpasswd unavailable`** from `ci_linux.sh` — install `tigervnc-tools`; it is
not pulled in by `tigervnc-common` any more.

**`A required package was not found` mentioning gnutls** — install
`libgnutls28-dev` *and* `pkg-config`, or drop `VNC_WITH_GNUTLS` (which loses TLS).

**`MSB8040` (Spectre-mitigated libraries)** — you enabled `VNC_SPECTRE` without
the matching VS component. Install it or leave the option off.

**CMake rejects `CMakePresets.json`** — your CMake predates 3.25 and cannot read
schema version 6. Upgrade, or configure without `--preset`.
