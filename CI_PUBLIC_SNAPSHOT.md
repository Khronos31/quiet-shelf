# Public CI snapshot of torne-userland

- **Source repository**: `Khronos31/torne-userland` (private at snapshot time).
- **Source commit**: `a1008acef6789ac154ffd75dbe479e74b2dfecd1` (main; PR5/PR7/PR6 integrated).
- **License**: GPL-2.0-only. The product `LICENSE` and `PROVENANCE.md` are preserved.
- **Dependency pin**: `Khronos31/dtv-userland-core` at
  `301c4593fc34702729750477d93f9d9e0051b0cb` (v1.0.1), fetched by CMake
  FetchContent from the public repository.
- **libusb**: 1.0.30, fetched by URL and SHA-256 from the libusb release page and
  built as a static library with udev disabled.
- **CI workflows**: `.github/workflows/ci.yml` runs the six automatic
  cross-platform jobs on pushes to `ci/torne-userland`. The adapted
  `.github/workflows/fr30-generation.yml` is manual-only (`workflow_dispatch`)
  and adds one job. Neither uses secrets, write tokens, private checkouts,
  releases, packages, schedules, or external uploads.
- This branch contains only source, documentation, and CI files from the
  allowlisted snapshot. No private git history, tokens, or device captures are
  copied.
- The workflow builds and runs non-hardware tests only. It never opens, claims,
  tunes, or streams from a tuner. Windows is cross-compiled and is not executed.
- CTest has six tests normally, including `torne_ts_profile_tests`, which
  verifies the instance spelling without relying on filesystem case sensitivity.
  The manual FR30 workflow supplies the pinned `fr30-elf` tools and runs eight
  tests, including `torne_fr30_generation` and `torne_widen_firmware`.
- This is a disposable CI copy, not the authoritative development branch.
  Fixes belong in the source repository first and are synchronized here after
  review.

The source snapshot contains the CLI framework, read-only USB discovery and
transport, read-only Widen recognition, the FR30 fixture-generation foundation,
and tracked Widen candidate images. The Widen images are not loaded by the
normal product build or the daemon, and their device behavior is unverified.
DiB8096 control and card support are not implemented in this snapshot.
