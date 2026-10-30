// SPDX-License-Identifier: GPL-2.0-only
#include "torned/daemon.h"

#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/records.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace torne::daemon {
namespace {

using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::error_string;
using dtv::userland::wire::DiscoveryScan;
using dtv::userland::wire::encode_discovery_json;
using dtv::userland::wire::discovery_json_size;

int exit_status(Error error) noexcept
{
    // Shared Core CLI contract; do not define a second exit-code table.
    return px4::userland::cli::px4_ts_exit_status(error);
}

bool write_all(std::FILE* output, const std::string& bytes) noexcept
{
    if (output == nullptr) return false;
    return std::fwrite(bytes.data(), 1U, bytes.size(), output) == bytes.size() &&
           std::fflush(output) == 0;
}

int run_list_json(const DaemonConfig& config, Backend* backend,
                  std::FILE* output, std::FILE* error) noexcept
{
    if (backend == nullptr) {
        std::fprintf(error,
            "torned: no hardware backend is available in this framework build; "
            "--list-json is unsupported\n");
        return exit_status(Error::UNSUPPORTED);
    }
    const auto scan = backend->discover(config);
    if (!scan) {
        std::fprintf(error, "torned: discovery failed: %s\n",
                     error_string(scan.error()));
        return exit_status(scan.error());
    }
    const DiscoveryScan& value = scan.value();
    const auto size = discovery_json_size(value);
    if (!size) {
        std::fprintf(error, "torned: discovery JSON rejected: %s\n",
                     error_string(size.error()));
        return exit_status(size.error());
    }
    std::string bytes(size.value(), '\0');
    const auto encoded = encode_discovery_json(
        value, MutableByteView{reinterpret_cast<std::uint8_t*>(bytes.data()),
                               bytes.size()});
    if (!encoded || encoded.value() != bytes.size()) {
        std::fprintf(error, "torned: discovery JSON encoding failed\n");
        return exit_status(encoded ? Error::INTERNAL : encoded.error());
    }
    if (!write_all(output, bytes)) {
        std::fprintf(error, "torned: could not write discovery JSON\n");
        return exit_status(Error::INTERNAL);
    }
    return 0;
}

}  // namespace

int run_daemon(const DaemonConfig& config, Backend* backend,
               std::FILE* output, std::FILE* error) noexcept
{
    if (config.list_json) return run_list_json(config, backend, output, error);

    if (backend == nullptr) {
        std::fprintf(error,
            "torned: no hardware backend is available in this framework build; "
            "refusing to start\n");
        return exit_status(Error::UNSUPPORTED);
    }
    auto family = backend->open_family(config);
    if (!family) {
        std::fprintf(error, "torned: hardware backend open failed: %s\n",
                     error_string(family.error()));
        return exit_status(family.error());
    }
    if (!family.value()) {
        std::fprintf(error, "torned: hardware backend returned no enclosure\n");
        return exit_status(Error::INTERNAL);
    }
    (void)family.value()->shutdown();

    // The listener Context, authority/worker wiring and IPC publication are the
    // next increment. Do not report a running daemon before they exist.
    std::fprintf(error,
        "torned: Core listener integration is not implemented in this framework "
        "build; the hardware backend was opened but nothing was published\n");
    return exit_status(Error::UNSUPPORTED);
}

}  // namespace torne::daemon
