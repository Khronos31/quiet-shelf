// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_DAEMON_CONFIG_H
#define TORNE_DAEMON_CONFIG_H

#include <dtv/userland/identity.h>

#include <string>

namespace torne::daemon {

struct DaemonConfig final {
    bool help = false;
    bool version = false;
    bool list_json = false;
    bool has_device = false;
    dtv::userland::identity::Selector device{};
    std::string instance;
    std::string runtime_directory;
};

struct ParseResult final {
    bool valid = false;
    DaemonConfig config{};
    std::string error;
};

// Parses the framework's torned entrance. Selector syntax and instance rules
// come from Core; this parser only decides which options the product exposes.
ParseResult parse_daemon_arguments(int argc, const char* const* argv) noexcept;

const char* daemon_usage() noexcept;

}  // namespace torne::daemon

#endif  // TORNE_DAEMON_CONFIG_H
