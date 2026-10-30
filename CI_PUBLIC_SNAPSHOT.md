# Public CI snapshot of torne-userland

- **Source repository**: `Khronos31/torne-userland` (private at snapshot time).
- **Source commit**: `f5bece97a356fd69d60b638151fffda93350b57e` (PR #3, open).
- **License**: GPL-2.0-only. The product `LICENSE` and `PROVENANCE.md` are preserved.
- **Dependency pin**: `Khronos31/dtv-userland-core` at
  `301c4593fc34702729750477d93f9d9e0051b0cb` (v1.0.1), fetched by CMake
  FetchContent from the public repository.
- **libusb**: 1.0.30, fetched by URL and SHA-256 from the libusb release page and
  built as a static library with udev disabled.
- **CI workflow**: `.github/workflows/ci.yml`, written only for this public
  snapshot. It does not use secrets, write tokens, private checkouts, releases,
  packages, schedules, or external uploads.
- This branch contains only source, documentation, and CI files from the
  allowlisted snapshot. No private git history, tokens, or device captures are
  copied.
- The workflow builds and runs non-hardware tests only. It never opens, claims,
  tunes, or streams from a tuner. Windows is cross-compiled and is not executed.
- This is a disposable CI copy, not the authoritative development branch.
  Fixes belong in the source repository first and are synchronized here after
  review.

The source snapshot contains only the CLI framework and read-only USB
discovery increment. USB transport, FR30 loading, DiB8096 control, and card
support are not implemented in this snapshot.
