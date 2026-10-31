# Public CI snapshot of torne-userland

- **Source repository**: `Khronos31/torne-userland` (private at snapshot time).
- **Source commit**: `2163dcbe32b07baa229612edd8536ec8602d5c52` (private `windows-pcsc-native` branch from main `ba35825`; Core pin update only, plus matching pin references).
- **License**: GPL-2.0-only. The product `LICENSE`, `PROVENANCE.md`, and the upstream attribution `NOTICE` are preserved.
- **Dependency pin**: `Khronos31/dtv-userland-core` at
  `548a04605d9e224be179547de30f590ff693a128` (Core main after PR144; shared Windows
  WinSCard SDK import fixture), fetched by CMake FetchContent from the public
  repository. It is not the final five-product pin.
- **libusb**: 1.0.30, fetched by URL and SHA-256 from the libusb release page and
  built as a static library with udev disabled.
- **CI workflows**: `.github/workflows/ci.yml` runs the six automatic
  cross-platform jobs on pushes to `ci/torne-userland`. The adapted
  `.github/workflows/fr30-generation.yml` is manual-only (`workflow_dispatch`)
  and adds one job. The branch-only `.github/workflows/windows-winscard-native.yml`
  runs one Windows native stage job on `ci/torne-winscard-native`. None uses
  secrets, write tokens, private checkouts, releases, packages, schedules, or
  external uploads.
- The branch-only `.github/workflows/macos-ifd.yml` installs Homebrew
  pcsc-lite only as a CI build-header input, records the version/header hash,
  and passes `../include/PCSC` through the existing Core cache variable. The
  generated IFD is not linked to libpcsclite/Homebrew, and users are not asked
  to install Homebrew. This remains a packaging/build-input step, not runtime
  support or OS registration.
- The branch-only `.github/workflows/windows-winscard-native.yml` is triggered only by
  `ci/torne-winscard-native`. It configures the Torne source with Core
  `DTV_USERLAND_BUILD_TESTING=ON` and `TORNE_BUILD_WINSCARD=ON`, builds
  `torne_pcsc_stage` plus the shared Core `windows_winscard_native_fixture_tests`
  and `windows_winscard_sdk_import_tests`, and runs both against the staged
  `WinSCard.dll`. The native fixture uses its own private temporary runtime and
  checks the Torne mock reader namespace/T=1/ATR/APDU; the SDK import fixture
  checks app-local module identity, 12 function imports plus
  `g_rgSCardT1Pci`, context creation, and the separate System32 provider.
  It does not register readers, modify System32, start a daemon/pcscd, open USB,
  or use a real card. The job is prepared but has not executed in this
  preparation.
- The branch-only `.github/workflows/macos-ifd.yml` is triggered only by
  `ci/torne-macos-ifd`. It builds the optional macOS IFD/bundle, checks Mach-O,
  exports, plist, notices, and dependencies, then probes `CFBundle` loading and
  the 10 IFDH function pointers with a temporary CoreFoundation client. It does
  not call IFDH operations, start a daemon/pcscd, register a service, open USB,
  or run the six normal jobs. The wrong `CFBundleExecutable` copy is a negative
  control. macOS actual service discovery and registration remain unverified.
- Before configure, the branch-only job also runs a diagnostic limited to
  `xcrun --show-sdk-path`: it prints `xcodebuild -version`, candidate
  `ifdhandler.h` paths/realpaths, and the count/first paths of a
  `find "$sdk" -name ifdhandler.h` scan. The SDK root is normalized to its
  real directory first; child symlinks are not followed. Missing/unresolved
  SDK roots report `ifdhandler_count=unknown`, not a false zero. This step does not add a header,
  include path, shim, fallback, or change the existing configure/build/load
  conditions. It is diagnostic output only, not evidence that a header exists.
- This branch contains only source, documentation, PC/SC packaging, and CI files from the
  allowlisted snapshot. No private git history, tokens, or device captures are
  copied.
- The workflow builds and runs non-hardware tests only. It never opens, claims,
  tunes, or streams from a tuner. The normal Windows jobs are cross-compiled and
  are not executed; the branch-only Windows native stage job is prepared but not
  executed in this preparation.
- CTest has seventeen tests normally. The Linux job additionally configures an
  optional `TORNE_BUILD_PCSC_IFD=ON` build with tests OFF and checks the staged
  IFD `.so`, notices, reader template, and ten `IFDH*` ABI symbols. The Windows
  cross job stages `WinSCard.dll` and checks its exports, imports, and notices.
  The branch-only Windows native job applies the shared Core mock and SDK import
  fixtures to the staged DLL. These are CI-only staging checks; no pcscd, OS
  reader registration, service, or real device I/O is performed. macOS keeps
  its normal build/tests and does not claim bundle loader/registration success.
- The manual FR30 workflow adds four tests (twenty-one total when enabled),
  including `torne_fr30_generation`, `torne_widen_firmware`,
  `torne_core1_firmware`, and `torne_core0_firmware`.
- This is a disposable CI copy, not the authoritative development branch.
  Fixes belong in the source repository first and are synchronized here after
  review.

PR22 adds the cold-start Widen clock-gate initialization (`src/product/clock_gate.*`, fake/CTest only). This is not evidence of real cold boot or F3 success; real hardware validation is a separate scout task.

The source snapshot includes the upstream `torne_drv` attribution notice for the DiB8096-derived files (coax and Linux DiBcom notices, with the px4_drv section scoped to upstream files not included here). It contains the CLI framework, USB transport/discovery, Widen, Core 1 and Core 0 host runtimes, DiB8096 C11 frontend, card UART/CardService adapters, the product daemon/family path, FR30 candidate images/fixtures, and optional PC/SC packaging. The normal build does not generate FR30 images; it uses the tracked Widen/Core 0/Core 1 images. CI does not open, claim, tune, or stream from a tuner; the CLI smoke may perform read-only USB enumeration. The direct `torne-ts --device usb:BUS-PORT` path is the product startup path that performs Widen/FR30/Core startup, tune, and TS on hardware; it is outside CI. The daemon listener/family connection and CardService adapter are included, but real device I/O remains outside CI. The card path is tested with fake UART/Core only. Optional PC/SC packaging is a CI-staged candidate; Linux pcscd/OS registration, macOS loader/registration, and real IFD/card operation remain unverified. OS PC/SC system registration is not included.
