// SPDX-License-Identifier: GPL-2.0-only
#include "torned/config.h"
#include "torned/daemon.h"
#include "usb/backend.h"

#include <cstdio>

#if defined(_WIN32)
#include <px4_windows_args.h>

#include <string>
#include <vector>
#endif

int main(int argc, char** argv)
{
    using torne::daemon::parse_daemon_arguments;
#if defined(_WIN32)
    const std::vector<std::string> owned_argv =
        px4::userland::cli::windows_argv_utf8(argc, argv);
    std::vector<const char*> argv_views;
    argv_views.reserve(owned_argv.size());
    for (const std::string& value : owned_argv) argv_views.push_back(value.c_str());
    const auto parsed = parse_daemon_arguments(
        static_cast<int>(argv_views.size()), argv_views.data());
#else
    const auto parsed = parse_daemon_arguments(
        argc, const_cast<const char* const*>(argv));
#endif
    if (!parsed.valid) {
        std::fprintf(stderr, "torned: %s\n", parsed.error.c_str());
        std::fputs(torne::daemon::daemon_usage(), stderr);
        return 2;
    }
    if (parsed.config.help) {
        std::fputs(torne::daemon::daemon_usage(), stdout);
        return 0;
    }
    if (parsed.config.version) {
        std::printf("torned %s\n", TORNE_USERLAND_PRODUCT_VERSION);
        return 0;
    }
    torne::usb::UsbBackend backend;
    return torne::daemon::run_daemon(parsed.config, &backend, stdout, stderr);
}
