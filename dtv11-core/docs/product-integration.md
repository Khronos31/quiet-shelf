<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Product integration

The fixture under `tests/fixture` is a small product directory that adds Core with
`add_subdirectory`, declares its product library from `lib/*.cpp`, and builds its
command from the Core template. From the Core repository root, run:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

To build the fixture as an independent product directory:

```sh
cmake -S tests/fixture -B build/fixture -DBUILD_TESTING=ON
cmake --build build/fixture
ctest --test-dir build/fixture --output-on-failure
```

The fixture's `CMakeLists.txt` supplies product name, command name, and version, then
calls `dtv_add_product`. Its `lib/devices.cpp` contains the `DeviceOps` table and the
model accessor used by the generated command. The accessor is pulled from the static
product archive because the generated Core main calls its C ABI symbol. Unlisted
callbacks are null and therefore not implemented. The CLI and option contract stays
in Core; products do not copy an option mapping table. `DEFAULT_MODEL` is embedded at
build time; the fixture leaves it empty so no device is selected implicitly. This
initial metadata-only CLI does not yet apply operational defaults.

A product repository uses the same function after adding its pinned Core checkout:

```cmake
cmake_minimum_required(VERSION 3.20)
project(siano_userland LANGUAGES C CXX)

add_subdirectory(external/dtv11-core core)
dtv_add_product(NAME siano
                COMMAND sianoctl
                VERSION 0.1.0
                DEFAULT_MODEL ""
                SOURCES lib/models.cpp)
```

The first model table in the fixture shows the source shape:

```cpp
std::span<const DeviceOps> product_devices()
{
    static constexpr std::array<DeviceOps, 2> devices{{
        {{"Fixture One", 0x1234, 0x0001}, nullptr, nullptr},
        {{"Fixture Two", 0x5678, 0x9abc}, nullptr, nullptr},
    }};
    return devices;
}
```

The table lives in a source file inside `libfoo.a`. The product also implements the
two C-typed model accessors called by the generated main; see
`tests/fixture/lib/devices.cpp` for the complete working file. The `DeviceOps` entries
use null for the initial open/close slots because this fixture has no hardware path.

`include/dtv/cli.h` is the C ABI consumed by the generated main and non-C++ clients.
It exposes only C-compatible types. `include/dtv/spi/device_ops.hpp` is the C++ product
SPI for model descriptors and nullable operation callbacks. Strings and callback
tables are borrowed from static storage and remain valid for the process lifetime.
The caller owns every buffer it passes to the C API; the API retains no caller-owned
memory. No C++ exception is part of the C ABI contract.

During development a product can pin this repository as a submodule at a reviewed
commit SHA. Set `CORE_SSH_URL` to the private Core remote in the local environment,
and set `CORE_SHA` to the reviewed commit SHA:

```sh
git submodule add "$CORE_SSH_URL" external/dtv11-core
git -C external/dtv11-core checkout "$CORE_SHA"
git add external/dtv11-core
```

The product build then adds that fixed checkout as a subdirectory before calling
`dtv_add_product`. For the later public quiet-shelf CI, copy the pinned Core files
into the public repository as ordinary files; do not use this private submodule URL.
The workflow in `.github/workflows/ci.yml` runs the same fixture build and external
contract matrix on Linux and Windows/MSVC when copied alongside those files.

This initial SPI has only model identity, VID:PID, and nullable open/close slots. It
does not define the complete hardware API. Future operations can be added to the SPI
as their Core behavior is implemented, without claiming that null callbacks work.

## License and provenance

The Core sources in this repository were written for dtv11 and do not copy code from
the read-only reference Core. Each source and documentation file carries the
GPL-2.0-or-later SPDX identifier. Any future third-party source must be checked for
compatible licensing before it is included.

## Output errors and SIGPIPE

The C API checks stdout writes and flushes and returns 70 when they fail. It does not
change process signal dispositions. The generated command ignores SIGPIPE in its
`main` on POSIX so a closed pipe becomes a normal write error and returns 70. An
in-process caller keeps control of its own SIGPIPE policy; with the default policy,
the host may terminate on a closed stdout pipe before the C API can return.

## Argument decision stages

The CLI sends the original argument vector into the Core C API. The Core handles
metadata before ordinary argument validation: help wins over version, and both win
over models and invalid arguments. Models is accepted only on its own. The PR147 L1
contract assigns unknown input error code 2; unsupported known hardware capability
is a later, distinct result (code 3). This increment implements no hardware operation
as a known command, so non-metadata arguments are unknown and rejected with code 2.
Once a command is added, the Core will first recognize it and then use the selected
device's callback table to decide whether that hardware supports it. Required runtime
inputs such as daemon firmware do not apply to metadata-only requests and are not
implemented here.
