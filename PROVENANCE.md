# Source provenance, licenses, and version pins

This repository is GPL-2.0-only; all upstream copyrights are retained.

## Vendor: mihalinn/torne_drv

- URL: https://github.com/mihalinn/torne_drv
- Immutable source commit: \`fc662902a3e4ad56566b00cae4a0ead922e3dd02\`
- License: GPL-2.0-only, as recorded by upstream LICENSE / NOTICE.
- Unmodified files in \`third_party/torne_drv\`:
  \`torne_core.h\`, \`torne_regs.h\`, \`torne_bridge.c\`,
  \`torne_fw_gen.h\`, \`torne_dib.h\`, \`torne_dib.c\`,
  \`torne_dib_tables_gen.h\`, \`torne_card.c\`, \`torne_card.h\`,
  \`LICENSE\` and \`NOTICE\`.
- \`torne_fw_gen.h\` is generated FR30 code, and
  \`torne_dib_tables_gen.h\` is the generated DiB8096 tables.
  **Do not mix these headers with a different C implementation version.**
- Upstream NOTICE attributes MB86H47A and ISO7816-related material to
  Till0196/coax; DiB hardware drivers to DiBcom/Linux through coax.
  Kernel character device sources originally derived from px4_drv are
  intentionally **not copied** into this user-space project.
- Upstream reference for coax:
  https://github.com/Till0196/coax (versioned generator sources).

## Shared common core

- URL: https://github.com/Khronos31/dtv-userland-core
- Nominal baseline: \`v0.1.0\`
- **Pinned commit, not moving tag:**
  \`32731433803f01903b4f198ddd94647c4d222ac0\`
- Source is brought in with CMake FetchContent, not copied into this project.
- MIT applies to generic Core source; some optional Core host adapters
  are GPL-2.0-only. This project is GPL-2.0-only.
- A future v1.0.0 tag may be re-pointed; update the pinned commit only
  following verification. The Core CMake package may still advertise
  version 0.1.0 even when a separate tag says v1.0.0.

## Excluded sources

- No denpa/AGPL-3.0 code or generated code is imported. Ideas from denpa
  may be independently implemented in a later, separately reviewed change.
- No Sony proprietary firmware or NeToRne proprietary-dependant modules
  are included.

All new files (CMake, product glue, tests, tools, documentation) carry
SPDX \`GPL-2.0-only\` where suitable. This document is a technical
attribution record, not legal advice.
