# REL-01 release candidate build

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

The output contains:

- `torne-userland-<version>-source.tar.gz`
- `torne-userland-<version>-linux-glibc-x86_64.tar.gz`
- `SHA256SUMS`

Use `--source-only` to manufacture and verify only the source archive and its
SHA-256 file; it still requires the fixed Core and libusb inputs and does not
build binaries. The source archive contains the fixed Core and libusb source
inputs and a `REBUILD.md`. The binary archive contains the three CLIs, the glibc IFD stage,
licenses/provenance, and build metadata. Other ABI targets are not built in
this increment. `SOURCE-METADATA.json` and `BUILD-METADATA.json` carry separate
`product_source` and `checkout` identities; `SOURCE-MAP.json` is an input map,
not part of its own hash. The archive's actual checkout commit is the one used
for `git archive`; the product mapping is not substituted for it.

The builder verifies archive contents, hashes, the IFD export set, ELF
dependencies, and runs `--version`/`--help` from the extracted binary archive.
The common ELF dependency guard applies to the three CLIs and the IFD. The
three CLIs additionally require a standard glibc x86_64 `PT_INTERP` (the
distro-standard `/lib64`, `/lib`, `/lib/x86_64-linux-gnu`, `/usr/lib64`,
`/usr/lib`, or `/usr/lib/x86_64-linux-gnu` loader path); a build-host-only
absolute loader path is rejected. It does not claim bit-for-bit
reproducibility, publish a tag/Release, upload an artifact, or test
hardware/OS integration.

## Dependency source

| input | fixed location |
| --- | --- |
| Core | `Khronos31/dtv-userland-core` at the CMake-defined commit |
| libusb | `https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2` |
| libusb SHA-256 | `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf` |

The source archive records the same locations and hashes in
`SOURCE-METADATA.json`.
