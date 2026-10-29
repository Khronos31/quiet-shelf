// SPDX-License-Identifier: GPL-2.0-only
// Provisional one-shot probe, not the eventual torned persistent service.
#include <torne/libusb_transport.h>
#include <torne/product_contract.h>
#include <torne/torne_device.h>

#include <dtv/userland/error.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {
using dtv::userland::Error;

void help()
{
    std::fprintf(stderr,
        "torne-probe (experimental, GPL-2.0-only)\n"
        "  --list-json                  read-only discovery, never claim the device\n"
        "  --resolve T27                offline channel-to-Hz validation\n"
        "  --tune T27 --device usb:1-2 --seconds 10   explicit bounded capture to stdout\n"
        "  --version                    print version and frozen Core commit\n");
}

int fail(const char* operation, Error error)
{
    std::fprintf(stderr, "%s: %s\n", operation, dtv::userland::error_string(error));
    return 1;
}

int tune(int argc, char** argv)
{
    if (argc < 3) { help(); return 2; }
    std::string_view channel(argv[2]);
    std::string_view selector;
    unsigned seconds = 10;
    bool seen_seconds = false, seen_selector = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--device" && !seen_selector && i + 1 < argc) {
            selector = argv[++i];
            seen_selector = true;
        } else if (arg == "--seconds" && !seen_seconds && i + 1 < argc) {
            const std::string text(argv[++i]);
            char* end = nullptr;
            errno = 0;
            const auto parsed = std::strtoul(text.c_str(), &end, 10);
            if (errno != 0 || *end != '\0' || parsed == 0 || parsed > 300) {
                std::fprintf(stderr, "--seconds must be in 1..300\n");
                return 2;
            }
            seconds = static_cast<unsigned>(parsed);
            seen_seconds = true;
        } else { help(); return 2; }
    }
    if (selector.empty()) {
        std::fprintf(stderr, "explicit --device usb:bus-port is required\n");
        return 2;
    }
    const auto frequency = torne::resolve_terrestrial_hz(channel);
    if (!frequency) return fail("channel", frequency.error());

    torne::LibusbTransport transport;
    const auto opened = transport.open(selector);
    if (!opened) return fail("USB claim", opened.error());
    const auto callbacks = transport.callbacks();
    torne::Device device(callbacks, &transport);
    const auto ready = device.initialize();
    if (!ready) return fail("initialize", ready.error());
    const auto tuned = device.tune(frequency.value());
    if (!tuned) return fail("tune", tuned.error());
    const auto state = device.status();
    if (!state) return fail("status", state.error());
    if (!state.value().locked) {
        std::fprintf(stderr, "tune has no terminal lock\n");
        return 1;
    }
    const auto started = device.start_stream();
    if (!started) return fail("start stream", started.error());
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    std::array<std::uint8_t, 65536> buffer{};
    int result = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto read = device.read_ts({buffer.data(), buffer.size()}, 200);
        if (!read) {
            if (read.error() == Error::TIMEOUT) continue;
            result = fail("bulk read", read.error());
            break;
        }
        if (read.value() != 0U &&
            std::fwrite(buffer.data(), 1, read.value(), stdout) != read.value()) {
            std::fprintf(stderr, "TS output failed\n");
            result = 1;
            break;
        }
    }
    const auto stopped = device.stop_stream();
    if (!stopped) result = fail("stop stream", stopped.error());
    const auto shut = device.shutdown();
    if (!shut) result = fail("shutdown", shut.error());
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::puts("torne-userland 0.1.0-dev; dtv-userland-core commit 32731433803f01903b4f198ddd94647c4d222ac0");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        help();
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--resolve") == 0) {
        const auto result = torne::resolve_terrestrial_hz(argv[2]);
        if (!result) return fail("channel", result.error());
        std::printf("%u\n", result.value());
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--list-json") == 0) {
        torne::LibusbTransport usb;
        bool complete = false;
        const auto observations = usb.enumerate(complete);
        if (!observations) return fail("USB enumerate", observations.error());
        const auto json = torne::discovery_json(observations.value(), complete);
        if (!json) return fail("Core Discovery JSON", json.error());
        std::fwrite(json.value().data(), 1, json.value().size(), stdout);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--tune") == 0)
        return tune(argc, argv);
    help();
    return 2;
}
