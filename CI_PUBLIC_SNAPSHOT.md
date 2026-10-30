# Public CI snapshot of torne-userland

- **Source repository**: `Khronos31/torne-userland` (private at snapshot time).
- **Source commit**: `6d2ed64567c7c782a29a84b3e639f178eeb854a2` (main; includes the integrated UART17, Core0 host14, and product CLI16 increments; PR18 is not included).
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
- CTest has thirteen tests normally, including `torne_ts_profile_tests`, which
  verifies the instance spelling without relying on filesystem case sensitivity.
  The manual FR30 workflow supplies the pinned `fr30-elf` tools and runs seventeen
  tests, including `torne_fr30_generation`, `torne_widen_firmware`,
  `torne_core1_firmware`, and `torne_core0_firmware`.
- This is a disposable CI copy, not the authoritative development branch.
  Fixes belong in the source repository first and are synchronized here after
  review.

The source snapshot includes the upstream `torne_drv` attribution notice for the DiB8096-derived files (coax and Linux DiBcom notices, with the px4_drv section scoped to upstream files not included here). It contains the CLI framework, USB transport/discovery, Widen, Core 1 and Core 0 host runtimes, DiB8096 C11 frontend, card UART boundary, the direct `torne-ts --device usb:BUS-PORT` product path, and FR30 candidate images/fixtures. The normal build does not generate FR30 images; it uses the tracked Widen/Core 0/Core 1 images. CI does not open, claim, tune, or stream from a tuner; the CLI smoke may perform read-only USB enumeration. The direct `torne-ts --device usb:BUS-PORT` path is the product startup path that performs Widen/FR30/Core startup, tune, and TS on hardware; it is outside CI. The daemon listener/family connection is not included in this snapshot (PR18 is not merged), and the card path stops at the UART boundary (ATR/T=1/CardService are not included).
