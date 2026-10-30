// SPDX-License-Identifier: GPL-2.0-only
// Developer live check for read-only Widen recognition. Built only with
// TORNE_BUILD_LIVE_USB_CHECK=ON. It opens/claims the selected device, reads the
// five patch words and the registered fixed-length image, and never writes,
// resets, detaches, or changes configuration.
#include "usb/libusb_io.h"
#include "usb/transport.h"
#include "widen/bridge.h"
#include "widen/recognition.h"

#include <dtv/userland/identity.h>

#include <cstdio>
#include <cstring>

namespace {

const char* state_name(torne::widen::WidenState state)
{
    switch (state) {
    case torne::widen::WidenState::pristine: return "PRISTINE";
    case torne::widen::WidenState::ours_active: return "OURS_ACTIVE";
    case torne::widen::WidenState::foreign_known: return "FOREIGN_KNOWN";
    case torne::widen::WidenState::partial_ours: return "PARTIAL_OURS";
    case torne::widen::WidenState::unknown: return "UNKNOWN";
    }
    return "?";
}

const char* stage_name(torne::widen::RecognitionStage stage)
{
    switch (stage) {
    case torne::widen::RecognitionStage::five_words: return "five_words";
    case torne::widen::RecognitionStage::header: return "header";
    case torne::widen::RecognitionStage::image: return "image";
    }
    return "?";
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
    auto opened = torne::usb::Transport::open(
        torne::usb::create_libusb_io(), location);
    if (opened.outcome.state != torne::usb::OpenState::ok ||
        opened.transport == nullptr) {
        std::fprintf(stderr, "open failed state=%d\n",
                     static_cast<int>(opened.outcome.state));
        return 3;
    }
    torne::widen::Bridge bridge(*opened.transport);
    std::size_t count = 0U;
    const torne::widen::RegisteredImage* registry =
        torne::widen::legacy_registry(count);
    const auto result = torne::widen::recognize_read_only(bridge, nullptr, registry, count);
    opened.transport.reset();
    std::printf("classified=%d state=%s stage=%s introduction_allowed=%d\n",
                result.classified ? 1 : 0, state_name(result.state),
                stage_name(result.stage), result.introduction_allowed ? 1 : 0);
    if (!result.classified) {
        std::printf("read_failure state=%d cause=%d transferred=%zu retried=%d\n",
                    static_cast<int>(result.failure.state),
                    static_cast<int>(result.failure.cause),
                    result.failure.transferred,
                    result.failure.retried ? 1 : 0);
        return 4;
    }
    return 0;
}
