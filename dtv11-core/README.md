<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# dtv11-userland-core

Shared core for the dtv11 userland products. It builds `libdtv.a` and provides the
generated command-line entry point and product CMake function.

See [product integration](docs/product-integration.md) for the fixture and build
commands, and [quiet-shelf CI](docs/quiet-shelf-ci.md) for the public snapshot
procedure. This initial increment implements metadata requests only; hardware and
daemon operations remain unimplemented.

This project is licensed under GPL-2.0-or-later; see [LICENSE](LICENSE).
