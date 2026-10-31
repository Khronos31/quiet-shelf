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

## Dependency source

| input | fixed location |
| --- | --- |
| Core | `Khronos31/dtv-userland-core` at the CMake-defined commit |
| libusb | `https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2` |
| libusb SHA-256 | `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf` |

The source archive records the same locations and hashes in
`SOURCE-METADATA.json`.
