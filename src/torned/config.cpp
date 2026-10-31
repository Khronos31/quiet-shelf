// SPDX-License-Identifier: GPL-2.0-only
#include "torned/config.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>

namespace torne::daemon {
namespace {

ParseResult invalid(const char* message) noexcept
{
    ParseResult result;
    result.error = message;
    return result;
}

}  // namespace

ParseResult parse_daemon_arguments(int argc, const char* const* argv) noexcept
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr)
        return invalid("invalid argument vector");
    ParseResult result;
    bool have_instance = false;
    bool have_runtime = false;
    bool have_fd = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view option(argv[index]);
        if (option == "-h" || option == "--help") {
            if (argc != 2) return invalid("--help cannot be combined");
            result.config.help = true;
            result.valid = true;
            return result;
        }
        if (option == "--version") {
            if (argc != 2) return invalid("--version cannot be combined");
            result.config.version = true;
            result.valid = true;
            return result;
        }
        if (option == "--list-json") {
            if (result.config.list_json) return invalid("duplicate --list-json");
            result.config.list_json = true;
            continue;
        }
        if (option != "--device" && option != "--instance" &&
            option != "--runtime-dir" && option != "--fd")
            return invalid("unknown argument");
        if (index + 1 >= argc || argv[index + 1] == nullptr)
            return invalid("option requires a value");
        const std::string_view value(argv[++index]);
        if (value.empty() || value.rfind("--", 0U) == 0U)
            return invalid("option requires a value");
        if (option == "--device") {
            if (result.config.has_device) return invalid("duplicate --device");
            const auto selector = dtv::userland::identity::parse_device_selector(value);
            if (!selector) return invalid("invalid --device selector");
            if (selector.value().kind !=
                dtv::userland::identity::SelectorKind::usb_topology)
                return invalid("serial selectors are not supported; use usb:BUS-PORT[.PORT...]");
            result.config.has_device = true;
            result.config.device = selector.value();
        } else if (option == "--instance") {
            if (have_instance) return invalid("duplicate --instance");
            if (!dtv::userland::identity::canonicalize_instance(value))
                return invalid("invalid --instance token");
            have_instance = true;
            result.config.instance.assign(value.data(), value.size());
        } else if (option == "--fd") {
            if (have_fd) return invalid("duplicate --fd");
            std::uint64_t parsed = 0U;
            const auto converted =
                std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
            if (converted.ec != std::errc{} ||
                converted.ptr != value.data() + value.size() ||
                parsed > static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max()))
                return invalid("--fd is invalid");
            have_fd = true;
            result.config.has_fd = true;
            result.config.fd = static_cast<int>(parsed);
        } else {
            if (have_runtime) return invalid("duplicate --runtime-dir");
            have_runtime = true;
            result.config.runtime_directory.assign(value.data(), value.size());
        }
    }
    if (result.config.list_json) {
        if (result.config.has_device || have_instance || have_runtime ||
            result.config.has_fd)
            return invalid("--list-json cannot be combined with startup options");
        result.valid = true;
        return result;
    }
    if (result.config.has_fd) {
        if (result.config.has_device)
            return invalid("--device cannot be combined with --fd");
        if (!have_instance)
            return invalid("--fd requires --instance; a borrowed descriptor has no USB path");
        result.valid = true;
        return result;
    }
    if (!result.config.has_device && !have_instance)
        return invalid("--device usb:BUS-PORT[.PORT...] or --instance is required");
    result.valid = true;
    return result;
}

const char* daemon_usage() noexcept
{
    return
        "usage:\n"
        "  torned --list-json\n"
        "  torned (--device usb:BUS-PORT[.PORT...] | --instance TOKEN) [--runtime-dir PATH]\n"
        "  torned --fd FD --instance TOKEN [--runtime-dir PATH]\n"
        "  torned --help | --version\n"
        "\n"
        "  Without --device the daemon selects the first discovered enclosure.\n"
        "  --fd takes a borrowed device descriptor (for example from termux-usb).\n"
        "  --list-json remains a read-only scan. Startup never fabricates a\n"
        "  receiver or card state when no usable enclosure is present.\n";
}

}  // namespace torne::daemon
