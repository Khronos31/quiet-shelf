# Public CI snapshot of torne-userland

- **Source repository**: Khronos31/torne-userland (private at snapshot time).
- **Source commit**: `ec9b2f7d1e735835529ff9febd959fe54a3865b5` (Draft PR #1).
- **License**: GPL-2.0-only, with upstream LICENSE and NOTICE preserved.
- **Dependency pin**: Khronos31/dtv-userland-core at `32731433803f01903b4f198ddd94647c4d222ac0`.
- **Public third-party upstream**: mihalinn/torne_drv at `fc662902a3e4ad56566b00cae4a0ead922e3dd02`.
- The exact upstream `driver/torne_dib.c` and `driver/torne_card.c` files are restored at CI time from the **public** upstream, then the two tracked GPL-compatible portability edits from the private Draft PR are applied. The resulting file hashes are checked against the private source blobs. These large upstream files are intentionally not duplicated in the branch.
- This branch contains only source and CI files. No .git history, private tokens, or device captures are copied.
- This workflow only compiles and runs non-hardware tests, never controls a tuner.

**Important:** Source publication here is authorized by the owner. GitHub Actions logs are public. This is a disposable CI copy, *not* the authoritative development branch. Fixes should be applied to the source repository first, then synchronized to this branch.

The included generated FR30 machine code remains subject to the corresponding-source issue identified by Opus (agent-workspace audit #6); a successful build does **not** resolve its source-distribution compliance for binaries.
