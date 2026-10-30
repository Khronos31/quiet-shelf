// SPDX-License-Identifier: GPL-2.0-only
// Developer live check for the USB transport. It is not a CTest and is never
// built unless TORNE_BUILD_LIVE_USB_CHECK=ON. It performs only open, claim,
// a standard device-descriptor read, a second-claim check, release and reopen.
// It does not reset, detach, change configuration, load firmware, tune, or
// transfer vendor payloads.
#include "usb/libusb_io.h"
#include "usb/transport.h"

#include <dtv/userland/identity.h>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

const char* open_state_name(torne::usb::OpenState state)
{
    switch (state) {
    case torne::usb::OpenState::ok: return "ok";
    case torne::usb::OpenState::not_found: return "not_found";
    case torne::usb::OpenState::busy: return "busy";
    case torne::usb::OpenState::access: return "access";
    case torne::usb::OpenState::unsupported: return "unsupported";
    case torne::usb::OpenState::disconnected: return "disconnected";
    case torne::usb::OpenState::io: return "io";
    case torne::usb::OpenState::invalid: return "invalid";
    }
    return "?";
}

const char* transfer_state_name(torne::usb::TransferState state)
{
    switch (state) {
    case torne::usb::TransferState::complete: return "complete";
    case torne::usb::TransferState::short_transfer: return "short";
    case torne::usb::TransferState::failed: return "failed";
    case torne::usb::TransferState::outcome_unknown: return "outcome_unknown";
    }
    return "?";
}

const char* transfer_cause_name(torne::usb::TransferCause cause)
{
    switch (cause) {
    case torne::usb::TransferCause::none: return "none";
    case torne::usb::TransferCause::timeout: return "timeout";
    case torne::usb::TransferCause::pipe: return "pipe";
    case torne::usb::TransferCause::no_device: return "no_device";
    case torne::usb::TransferCause::overflow: return "overflow";
    case torne::usb::TransferCause::io: return "io";
    case torne::usb::TransferCause::unsupported: return "unsupported";
    case torne::usb::TransferCause::access: return "access";
    case torne::usb::TransferCause::busy: return "busy";
    case torne::usb::TransferCause::no_memory: return "no_memory";
    case torne::usb::TransferCause::invalid: return "invalid";
    case torne::usb::TransferCause::other: return "other";
    }
    return "?";
}

bool read_device_descriptor(torne::usb::Transport& transport)
{
    std::uint8_t descriptor[18] = {};
    const auto result = transport.control_in(
        0x80U, 0x06U, 0x0100U, 0U, sizeof(descriptor), descriptor, 1000U,
        torne::usb::ControlKind::pure_read);
    std::printf("control: state=%s cause=%s transferred=%zu retried=%d\n",
                transfer_state_name(result.state),
                transfer_cause_name(result.cause), result.transferred,
                result.retried ? 1 : 0);
    if (result.state != torne::usb::TransferState::complete ||
        result.transferred != sizeof(descriptor))
        return false;
    if (descriptor[0] != 18U || descriptor[1] != 1U)
        return false;
    const std::uint16_t vid =
        static_cast<std::uint16_t>(descriptor[8] | (descriptor[9] << 8));
    const std::uint16_t pid =
        static_cast<std::uint16_t>(descriptor[10] | (descriptor[11] << 8));
    std::printf("descriptor: idVendor=0x%04x idProduct=0x%04x\n", vid, pid);
    return vid == 0x054cU && pid == 0x04b2U;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::printf("usage: %s --device usb:BUS-PORT[.PORT...]\n", argv[0]);
        return 0;
    }
    if (argc != 3 || std::strcmp(argv[1], "--device") != 0) {
        std::fprintf(stderr, "usage: %s --device usb:BUS-PORT[.PORT...]\n", argv[0]);
        return 2;
    }
    const auto selector = dtv::userland::identity::parse_device_selector(argv[2]);
    torne::usb::DeviceLocation location;
    if (!selector || !torne::usb::location_from_selector(selector.value(), location)) {
        std::fprintf(stderr, "invalid USB location\n");
        return 2;
    }

    auto first = torne::usb::Transport::open(
        torne::usb::create_libusb_io(), location);
    std::printf("open: %s\n", open_state_name(first.outcome.state));
    if (first.outcome.state != torne::usb::OpenState::ok ||
        first.transport == nullptr)
        return 3;
    if (!read_device_descriptor(*first.transport)) return 4;

    auto second = torne::usb::Transport::open(
        torne::usb::create_libusb_io(), location);
    std::printf("second open while claimed: %s\n",
                open_state_name(second.outcome.state));
    const bool exclusive = second.outcome.state == torne::usb::OpenState::busy;
    second.transport.reset();
    first.transport.reset();

    auto reopened = torne::usb::Transport::open(
        torne::usb::create_libusb_io(), location);
    std::printf("reopen: %s\n", open_state_name(reopened.outcome.state));
    if (reopened.outcome.state != torne::usb::OpenState::ok ||
        reopened.transport == nullptr)
        return 5;
    if (!read_device_descriptor(*reopened.transport)) return 6;
    reopened.transport.reset();

    if (!exclusive) {
        std::fprintf(stderr,
            "second claim was not reported busy; single-owner is not enforced\n");
        return 7;
    }
    std::puts("transport live check: PASS");
    return 0;
}
