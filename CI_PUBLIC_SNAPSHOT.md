# Public CI snapshot of torne-userland

- **Source repository**: `Khronos31/torne-userland` (private at snapshot time).
- **Source commit**: `30530f2c0d94e76091adeaea5156b0d33c73d945` (**PR21 candidate**, not yet main; base main `4606ef12`; Core/family, CardService, and optional PC/SC packaging).
- **License**: GPL-2.0-only. The product `LICENSE`, `PROVENANCE.md`, and the upstream attribution `NOTICE` are preserved.
- **Dependency pin**: `Khronos31/dtv-userland-core` at
  `1d5569ee303204cbb2d660517152d8a5b5878041` (Core141 intermediate pin), fetched by CMake
  FetchContent from the public repository. It is not the final five-product pin.
- **libusb**: 1.0.30, fetched by URL and SHA-256 from the libusb release page and
  built as a static library with udev disabled.
- **CI workflows**: `.github/workflows/ci.yml` runs the six automatic
  cross-platform jobs on pushes to `ci/torne-userland`. The adapted
  `.github/workflows/fr30-generation.yml` is manual-only (`workflow_dispatch`)
  and adds one job. Neither uses secrets, write tokens, private checkouts,
  releases, packages, schedules, or external uploads.
- This branch contains only source, documentation, PC/SC packaging, and CI files from the
  allowlisted snapshot. No private git history, tokens, or device captures are
  copied.
- The workflow builds and runs non-hardware tests only. It never opens, claims,
  tunes, or streams from a tuner. Windows is cross-compiled and is not executed.
- CTest has sixteen tests normally. The Linux job additionally configures an
  optional `TORNE_BUILD_PCSC_IFD=ON` build with tests OFF and checks the staged
  IFD `.so`, notices, reader template, and ten `IFDH*` ABI symbols. The Windows
  cross job stages `WinSCard.dll` and checks its exports, imports, and notices.
  These are CI-only staging checks; no pcscd, OS reader registration, service,
  or real device I/O is performed. macOS keeps its normal build/tests and does
  not claim bundle loader/registration success.
- The manual FR30 workflow adds four tests (twenty total when enabled),
  including `torne_fr30_generation`, `torne_widen_firmware`,
  `torne_core1_firmware`, and `torne_core0_firmware`.
- This is a disposable CI copy, not the authoritative development branch.
  Fixes belong in the source repository first and are synchronized here after
  review.

The source snapshot includes the upstream `torne_drv` attribution notice for the DiB8096-derived files (coax and Linux DiBcom notices, with the px4_drv section scoped to upstream files not included here). It contains the CLI framework, USB transport/discovery, Widen, Core 1 and Core 0 host runtimes, DiB8096 C11 frontend, card UART/CardService adapters, the product daemon/family path, FR30 candidate images/fixtures, and optional PC/SC packaging. The normal build does not generate FR30 images; it uses the tracked Widen/Core 0/Core 1 images. CI does not open, claim, tune, or stream from a tuner; the CLI smoke may perform read-only USB enumeration. The direct `torne-ts --device usb:BUS-PORT` path is the product startup path that performs Widen/FR30/Core startup, tune, and TS on hardware; it is outside CI. The daemon listener/family connection and CardService adapter are included, but real device I/O remains outside CI. The card path is tested with fake UART/Core only. Optional PC/SC packaging is a CI-staged candidate; Linux pcscd/OS registration, macOS loader/registration, and real IFD/card operation remain unverified. OS PC/SC system registration is not included.
