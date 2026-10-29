# torne-userland architecture (Core v0.1.0 frozen)

- Frozen shared Core commit: \`32731433803f01903b4f198ddd94647c4d222ac0\`.
- \`torne_hardware\`: GPL-compatible C chip driver from mihalinn/torne_drv.
  It owns MB86H47A memory/FR30, DiB8096, and TS ring, but **no USB enumeration**.
- \`torne_contract\`: native Core \`dtv::tuning\`, \`dtv::identity\`, \`dtv::json\`.
  No duplicate channel table or JSON serializer. No hardware side effects.
- \`torne_device\`: one physical receiver, bridge, and tuner, with injected
  \`torne_ops\` transport/time callbacks. No direct libusb dependency.
- \`torne_usb\` (optional): concrete libusb ownership, topology, and transport.
- \`torne-probe\`: provisional one-shot CLI. **Not a persistent daemon.**

## Boundaries and contracts

\`--list-json\` must never claim/open a device. It reads descriptors using
libusb and passes observations to the Core's v1 discovery JSON projector.
No serial exists on the tested unit; \`serial_unique\` must remain \`null\`,
not an invented true/false value. Device selection uses Core's \`usb:\` grammar.

Core v0.1.0 \`identity::Product\` only knows PX4/ASICEN/PIX/Siano. The
torne product must **not** be falsely encoded as one of those four to produce
an auto instance name. Parsing a USB selector itself is available; a genuine
torne product-ID encoding is pending Core support. This is an explicit
compatibility gap, not an invitation to fork Core or silently change its ABI.

Core v0.1.0 \`dtv::receiver\`, \`dtv::capture\` and the product host adapters
are optional and do not provide USB producers. This first implementation
does not pretend they are wired to the service until the physical backend,
stream ownership, and cleanup boundaries have tests.

The four added bytes of 192-byte incoming records are **uninterpreted**:
T27 tests of the existing coax stack found them all zero. Do not claim to
expose an arrival timestamp or BDAV extra header.

## CLI roadmap

Phase 1: \`torne-probe --list-json\`, \`--tune T27 --seconds 10
--device usb:1-2\` for bounded hardware testing only. No spontaneous
device reset, no hardware access during discovery.

Phase 2: \`torned\` + \`tornectl\` with Core's ownership, status, control,
card, and capture contracts after product-specific integration. The v1 Core
contains **parts**, not a ready-made cross-product daemon.

## Safety

Do not use USB test commands while the device is owned by the local Codex
hardware test. The \`torne-probe\` tune command needs explicit permission
and an exclusive USB claim. No actual tuner operation was run while
preparing this implementation branch.
