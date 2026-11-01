# Linux release candidate build

This increment adds the first release-candidate builder to the private product
tree. It is not a published release and does not add a public CI/release
workflow.

## Inputs

- Product commit: the clean `HEAD` of this checkout.
- Public synthetic checkout: if `SOURCE-MAP.json` is present at the repository
  root, it records the fixed product source repository/commit/tree. The helper
  records that as `product_source` and separately records the actual checkout
  repository/commit/tree. Without the map, both identities are the actual
  checkout.
- Core: the commit fixed by `cmake/TorneCoreDependency.cmake`; pass a checkout
  at that exact commit with `--core-source`.
- libusb: 1.0.30, fixed by `cmake/TorneLibusb.cmake`; the builder verifies the
  SHA-256. Use `--libusb-source-archive` for an offline/cached archive.

## Build

```sh
scripts/release/build-candidate.sh \
  --output /tmp/torne-release \
  --core-source /path/to/dtv-userland-core \
  --libusb-source-archive /path/to/libusb-1.0.30.tar.bz2
```

The default output contains:

- `torne-userland-<version>-source.tar.gz`
- `torne-userland-<version>-linux-glibc-x86_64.tar.gz`
- `SHA256SUMS`

Use `--source-only` to manufacture and verify only the source archive and its
SHA-256 file; it still requires the fixed Core and libusb inputs and does not
build binaries. The source archive contains the fixed Core and libusb source
inputs and a `REBUILD.md`. `SOURCE-METADATA.json` and `BUILD-METADATA.json`
carry separate `product_source` and `checkout` identities; `SOURCE-MAP.json`
is an input map, not part of its own hash. The archive's actual checkout commit
is the one used for `git archive`; the product mapping is not substituted for
it.

The builder verifies archive contents, hashes, the IFD export set, ELF
identity/dependencies, and runs `--version`/`--help` from extracted binary
archives when the selected toolchain is runnable on the build host or supplies
an explicit runner. The default glibc x86_64 CLI guard requires a standard
`PT_INTERP` (the distro-standard `/lib64`, `/lib`, `/lib/x86_64-linux-gnu`,
`/usr/lib64`, `/usr/lib`, or `/usr/lib/x86_64-linux-gnu` loader path); a
build-host-only absolute loader path is rejected. It does not claim bit-for-bit
reproducibility, publish a tag/Release, upload an artifact, or test
hardware/OS integration.

## Linux 4-archive matrix

`--linux-matrix` selects all four product-spec Linux archives; repeat
`--linux-platform` to select a subset:

- `linux-glibc-x86_64`
- `linux-glibc-aarch64`
- `linux-musl-x86_64`
- `linux-musl-aarch64`

A non-default or multi-platform build requires `--toolchain-config`, a
schema-1 JSON file with a `platforms` object. Each platform entry names `cc`
and `cxx`, and may set `c_flags`, `cxx_flags`, `exe_linker_flags`,
`shared_linker_flags`, `sysroot`, `cmake_system_name`, `system_processor`,
`c_compiler_target`, `cxx_compiler_target`,
`runner`, `run_checks`, `emulated`, `build_kind`, and `env` (a string map for
host toolchain/assembler libraries). `runner` is an argv prefix used only for
extracted `--version`/`--help` checks (for example a
private QEMU); it is recorded as emulated when `emulated` is true. The helper
rejects `libpcsclite`, libusb, libstdc++, libgcc_s, libudev, and libc++ in
ELF `DT_NEEDED`. musl CLIs must be fully static; glibc CLIs must use a
standard loader alias; both IFD variants must export the ten `IFDH*` symbols.
Every binary archive is built from the source archive produced by the same
invocation, not from a saved stage.

```sh
scripts/release/build-candidate.sh \
  --output /tmp/torne-release-linux \
  --core-source /path/to/dtv-userland-core \
  --libusb-source-archive /path/to/libusb-1.0.30.tar.bz2 \
  --linux-matrix \
  --toolchain-config /path/to/linux-matrix-toolchains.json
```

## Android / Termux 3 ABI

`--android-matrix` manufactures the three product-spec Android archives from
the same source archive:

- `torne-userland-<version>-android-armv7a.tar.gz`
- `torne-userland-<version>-android-aarch64.tar.gz`
- `torne-userland-<version>-android-x86_64.tar.gz`

Each archive contains the three CLIs, `bin/torne-termux`, product/Core/libusb
notices, the NDK `NOTICE`, `NOTICE.toolchain`, and `source.properties`, the
FR30 source/generated materials, `BUILD-METADATA.json`, and `SHA256SUMS`. An
Android archive does not contain the PC/SC IFD or WinSCard DLL.

The Android toolchain entry supplies the fixed NDK root. `cc`/`cxx` are then
taken from the NDK’s LLVM prebuilt tree, and the helper passes the NDK CMake
toolchain, ABI, `android-26`, `c++_static`, and the Android release linker
defaults `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`. Explicit
`exe_linker_flags`/`shared_linker_flags` override those defaults. Explicit
`c_flags`/`cxx_flags` are passed to CMake and recorded in build metadata. NDK
r27 uses its `darwin-x86_64` prebuilt on both Intel and Apple Silicon Macs. For
example:

```json
{
  "schema": 1,
  "platforms": {
    "android-aarch64": {
      "ndk": "/path/to/android-ndk-r27b",
      "android_platform": "android-26",
      "android_stl": "c++_static"
    }
  }
}
```

Repeat the platform entry for `android-armv7a` and `android-x86_64`, or use
`--android-platform` for a subset. NDK r27 is required; the private manufacture
uses `27.1.12297006` and records the exact revision.

```sh
scripts/release/build-candidate.sh \
  --output /tmp/torne-release-android \
  --core-source /path/to/dtv-userland-core \
  --libusb-source-archive /path/to/libusb-1.0.30.tar.bz2 \
  --android-matrix \
  --toolchain-config /path/to/android-toolchains.json
```

The helper builds the source archive once and then all selected ABIs from that
same archive. It strips the Android CLIs with the NDK `llvm-strip`, checks
PIE/machine/interpreter, the `libc.so`/`libm.so`/`libdl.so`/`liblog.so`
allowlist, 16 KiB PT_LOAD alignment, RELRO/BIND_NOW, no RPATH/RUNPATH, no debug
or symbol sections, no dynamic libusb/libc++/host loader, and static libusb
symbols in `torned`/`torne-ts`. It verifies inner/outer SHA-256 after extraction.
Android binaries are not executed on the build host; device help/version is a
separate check. The helper does not run `termux-usb`, open USB, or register a
reader.

## macOS darwin-arm64

`--darwin-arm64` builds the native arm64 3 CLIs and the candidate IFD bundle on
macOS. The output archive is
`torne-userland-<version>-darwin-arm64.tar.gz`; the IFD is under
`ifd/torne-userland-ifd.bundle`, with `Info.plist`, the bundle executable,
COPYING/provenance, and the product/Core license copies. `ifd/torne.reader.conf.in`
is included as the reader configuration template. The helper checks
Mach-O arm64 identity, Apple system framework/libc++/libSystem dependencies,
the bundle executable/plist binding, the 10 IFDH symbols, and native
`--version`/`--help`. It does not register the bundle with the OS PC/SC
service.

Darwin defaults to the system C++ runtime: omitting `exe_linker_flags` and
`shared_linker_flags` no longer applies the Linux `-static-libstdc++
-static-libgcc` defaults. Linux and Windows keep their existing static-runtime
defaults. The product-managed macOS toolchain entry is
`scripts/release/macos-toolchain.json`; docs and the existing macOS Release job
use this one file instead of separate inline values. It supplies only the
build-only PC/SC header location:

```json
{
  "schema": 1,
  "platforms": {
    "darwin-arm64": {
      "cc": "/usr/bin/cc",
      "cxx": "/usr/bin/c++",
      "cmake_args": [
        "-DDTV_USERLAND_PCSC_IFD_INCLUDE_DIR=/opt/homebrew/opt/pcsc-lite/include/PCSC"
      ],
      "system_processor": "arm64",
      "run_checks": true,
      "build_kind": "native"
    }
  }
}
```

The local and CI entry share the same helper and config:

```sh
scripts/release/build-candidate.sh \
  --output /tmp/torne-release-macos \
  --core-source /path/to/dtv-userland-core \
  --libusb-source-archive /path/to/libusb-1.0.30.tar.bz2 \
  --darwin-arm64 \
  --toolchain-config scripts/release/macos-toolchain.json
```

The CI entry for the existing macOS Release job is
`scripts/release/ci-macos-release.sh`. It reads the fixed Core commit from
`cmake/TorneCoreDependency.cmake` and the libusb version/URL/SHA from
`cmake/TorneLibusb.cmake`, verifies those inputs, and then invokes the same
helper/config. It manufactures the source archive and the Mac archive in the
same run, verifies inner/outer SHA-256, extracts the archive, checks
`--version`/`--help`, and inspects the IFD bundle/plist, the 10 `IFDH*` entries,
system-only dependencies, and the notice/provenance files. The job does not
upload the artifact or register a reader.

In the existing 8-job macOS Release job, add only:

```yaml
- name: Install build-only pcsc-lite headers
  if: matrix.config == 'Release'
  run: brew install pcsc-lite

- name: Manufacture and verify macOS release archive
  if: matrix.config == 'Release'
  run: scripts/release/ci-macos-release.sh
```

No new job, matrix, upload, Release, secret, or write permission is added. The
Core commit is read from `cmake/TorneCoreDependency.cmake` rather than copied as
a literal.

## Windows x86_64 (llvm-mingw UCRT)

`--windows-x86_64` builds the 3 CLIs and the app-local WinSCard DLL with the
fixed llvm-mingw/UCRT toolchain. The output archive is
`torne-userland-<version>-windows-x86_64.zip`; `WinSCard.dll` is under
`winscard/` with COPYING, adapter README, Core license, and the MinGW runtime
notice. The helper checks COFF x86-64 identity, system/UCRT imports, absence
of libusb/libstdc++/libgcc/libwinpthread runtime DLLs, and the 13 WinSCard
exports. The zip is not executed on Linux; device-free execution is a separate
native-host check.

The toolchain entry uses `cmake_system_name: Windows`, `readobj`, and the
fixed compiler paths. The Windows package does not modify System32 or register
readers.

## Reusing a fixed source archive

Pass `--source-archive` to build from an already-manufactured source archive
instead of creating a new one from the current checkout. The helper reads
`SOURCE-METADATA.json` from the archive, preserves the archive in the output,
and records its SHA-256 in `BUILD-METADATA.json`. This is the intended way to
build the macOS and Windows packages from the same fixed source archive
without pretending an old stage is a new-source build.

## Dependency source

| input | fixed location |
| --- | --- |
| Core | `Khronos31/dtv-userland-core` at the CMake-defined commit |
| libusb | `https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2` |
| libusb SHA-256 | `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf` |

The source archive records the same locations and hashes in
`SOURCE-METADATA.json`.
