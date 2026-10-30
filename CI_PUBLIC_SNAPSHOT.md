# Public CI snapshot of torne-userland

- **Source repository**: `Khronos31/torne-userland` (private at snapshot time).
- **Source commit**: `4606ef12dc98c5287ae822f47d3928cc9ff19391` (main; includes the integrated CLI, Core0 host, Core/family, and CardService increments).
- **License**: GPL-2.0-only. The product `LICENSE`, `PROVENANCE.md`, and the upstream attribution `NOTICE` are preserved.
- **Dependency pin**: `Khronos31/dtv-userland-core` at
  `9d8e02c4f113568012e257ed46d72c9a55751958` (intermediate Card/T=1 development pin), fetched by CMake
  FetchContent from the public repository. It is not the final five-product pin.
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
- CTest has sixteen tests normally, including `torne_product_daemon_tests`,
  `torne_product_core_family_tests`, and `torne_card_service_tests`. The manual
  FR30 workflow supplies the pinned `fr30-elf` tools and runs twenty tests,
  including `torne_fr30_generation`, `torne_widen_firmware`,
  `torne_core1_firmware`, and `torne_core0_firmware`.
- This is a disposable CI copy, not the authoritative development branch.
  Fixes belong in the source repository first and are synchronized here after
  review.

The source snapshot includes the upstream `torne_drv` attribution notice for the DiB8096-derived files (coax and Linux DiBcom notices, with the px4_drv section scoped to upstream files not included here). It contains the CLI framework, USB transport/discovery, Widen, Core 1 and Core 0 host runtimes, DiB8096 C11 frontend, card UART/CardService adapters, the product daemon/family path, and FR30 candidate images/fixtures. The normal build does not generate FR30 images; it uses the tracked Widen/Core 0/Core 1 images. CI does not open, claim, tune, or stream from a tuner; the CLI smoke may perform read-only USB enumeration. The direct `torne-ts --device usb:BUS-PORT` path is the product startup path that performs Widen/FR30/Core startup, tune, and TS on hardware; it is outside CI. The daemon listener/family connection and card CardService adapter are included, but real device I/O remains outside CI. The card path is tested with fake UART/Core only. OS PC/SC packaging is not included.
