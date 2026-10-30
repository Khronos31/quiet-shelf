# Public CI snapshot of torne-userland

- **Source repository**: `Khronos31/torne-userland` (private at snapshot time).
- **Source commit**: `e67587c330105607a751806e637a3584a003015a` (PR #15 head; base main `1d13e2a`; unmerged).
- **License**: GPL-2.0-only. The product `LICENSE`, `PROVENANCE.md`, and the upstream attribution `NOTICE` are preserved.
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
- CTest has ten tests normally, including `torne_ts_profile_tests`, which
  verifies the instance spelling without relying on filesystem case sensitivity.
  The manual FR30 workflow supplies the pinned `fr30-elf` tools and runs fourteen
  tests, including `torne_fr30_generation`, `torne_widen_firmware`,
  `torne_core1_firmware`, and `torne_core0_firmware`.
- This is a disposable CI copy, not the authoritative development branch.
  Fixes belong in the source repository first and are synchronized here after
  review.

The source snapshot includes the upstream `torne_drv` attribution notice for the DiB8096-derived files (coax and Linux DiBcom notices, with the px4_drv section scoped to upstream files not included here). It contains the CLI framework, read-only USB discovery and
transport, Widen recognition/loader, Core 1 host runtime and I2C backend, the
DiB8096 C11 frontend, and the FR30 generation foundation with Widen, Core 1,
and Core 0 candidate images. These components are offline or unverified on the
device; the candidate images are not automatically loaded by the normal
product build or daemon. Card support, tuning/TS integration, and the daemon
listener are not implemented in this snapshot.
