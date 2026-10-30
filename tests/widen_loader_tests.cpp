// SPDX-License-Identifier: GPL-2.0-only
#include "fr30/generated/widen-images.h"
#include "usb/transport.h"
#include "widen/bridge.h"
#include "widen/loader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace {

namespace usb = torne::usb;
namespace w = torne::widen;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

constexpr std::array<std::uint32_t, 5U> kPatchAddresses{
    0x0E9AC0U, 0x0E9AC4U, 0x0E9E0CU, 0x0E9E10U, 0x0E9D94U};
constexpr std::array<std::uint32_t, 5U> kPristineWords{
    0x9B800000U, 0xAA04E40FU, 0x9B800000U, 0xAA04E40DU, 0xB4279F8DU};
constexpr std::uint32_t kBank0 = 0x0E1000U;
constexpr std::uint32_t kBank1 = 0x0E1800U;
constexpr std::uint32_t kProbe = 0x017A3FC0U;

usb::DeviceSummary valid_summary()
{
    usb::DeviceSummary summary;
    summary.vid = 0x054cU;
    summary.pid = 0x04b2U;
    summary.num_configurations = 1U;
    summary.configuration_value = 1U;
    summary.num_interfaces = 1U;
    summary.interface_zero.number = 0U;
    summary.interface_zero.alternate = 0U;
    summary.interface_zero.interface_class = 0xffU;
    summary.interface_zero.endpoint_count = 4U;
    summary.interface_zero.endpoints[0] = {0x81U, 0x02U, 512U, 0U};
    summary.interface_zero.endpoints[1] = {0x02U, 0x02U, 512U, 0U};
    summary.interface_zero.endpoints[2] = {0x83U, 0x03U, 64U, 1U};
    summary.interface_zero.endpoints[3] = {0x84U, 0x02U, 512U, 0U};
    return summary;
}

usb::DeviceLocation location()
{
    usb::DeviceLocation location;
    location.bus = 1U;
    location.port_count = 1U;
    location.ports[0] = 2U;
    return location;
}

usb::TransferOutcome complete(std::size_t length = 4U)
{
    return usb::TransferOutcome{usb::TransferState::complete,
                                usb::TransferCause::none, length, false};
}

usb::TransferOutcome failure(usb::TransferState state, usb::TransferCause cause,
                             std::size_t transferred = 0U)
{
    return usb::TransferOutcome{state, cause, transferred, false};
}

struct Request final {
    bool in = false;
    std::uint32_t address = 0U;
    std::uint16_t length = 0U;
    std::array<std::uint8_t, 64U> data{};
};

class FakeDevice final : public usb::UsbIo {
public:
    FakeDevice() : summary(valid_summary()) {}
    ~FakeDevice() noexcept override = default;

    usb::DeviceSummary summary;
    std::map<std::uint32_t, std::uint8_t> memory;
    std::map<std::uint32_t, std::uint8_t> read_xor;
    std::map<std::uint32_t, std::uint8_t> read_xor_after_patches;
    std::vector<Request> requests;
    std::function<std::optional<usb::TransferOutcome>(const Request&, int)> in_hook;
    std::function<std::optional<usb::TransferOutcome>(const Request&, int)> out_hook;
    bool patches_applied = false;
    bool store_only_first_word_of_block = false;
    int in_calls = 0;
    int out_calls = 0;

    void load_word(std::uint32_t address, std::uint32_t value)
    {
        for (std::size_t index = 0U; index < 4U; ++index) {
            memory[address + static_cast<std::uint32_t>(index)] =
                static_cast<std::uint8_t>(value >> (24U - index * 8U));
        }
    }

    void load_bytes(std::uint32_t base, const std::uint8_t* bytes,
                    std::size_t length)
    {
        for (std::size_t index = 0U; index < length; ++index)
            memory[base + static_cast<std::uint32_t>(index)] = bytes[index];
    }

    std::uint32_t word(std::uint32_t address) const
    {
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const auto found = memory.find(address + static_cast<std::uint32_t>(index));
            value = (value << 8U) |
                    (found == memory.end() ? 0U : found->second);
        }
        return value;
    }

    std::size_t writes_in(std::uint32_t begin, std::uint32_t end) const
    {
        std::size_t count = 0U;
        for (const Request& request : requests) {
            if (!request.in && request.address >= begin && request.address < end)
                ++count;
        }
        return count;
    }

    usb::OpenOutcome open(const usb::DeviceLocation&, usb::DeviceSummary& output) noexcept override
    {
        output = summary;
        return usb::OpenOutcome{usb::OpenState::ok, usb::TransferCause::none};
    }

    usb::OpenOutcome claim_interface(std::uint8_t) noexcept override
    {
        return usb::OpenOutcome{usb::OpenState::ok, usb::TransferCause::none};
    }

    usb::TransferOutcome control_in(std::uint8_t request_type,
                                    std::uint8_t request,
                                    std::uint16_t value,
                                    std::uint16_t index,
                                    std::uint16_t length,
                                    std::uint8_t* data,
                                    std::uint32_t) noexcept override
    {
        Request record;
        record.in = true;
        record.address = ((static_cast<std::uint32_t>(value) & 0xFFU) << 24U) |
                         ((static_cast<std::uint32_t>(value) & 0xFF00U) << 8U) |
                         index;
        record.length = length;
        requests.push_back(record);
        ++in_calls;
        if (request_type != 0xC0U || request != 0xBCU || data == nullptr ||
            length < 4U || length > 64U || (length % 4U) != 0U ||
            (record.address & 0x03U) != 0U ||
            (!patches_applied &&
             (length != 4U || (record.address >> 24U) != 0U)))
            return failure(usb::TransferState::failed, usb::TransferCause::invalid);
        if (in_hook) {
            const auto injected = in_hook(record, in_calls);
            if (injected) return *injected;
        }
        for (std::uint16_t byte = 0U; byte < length; ++byte) {
            const std::uint32_t address = record.address + byte;
            const auto found = memory.find(address);
            std::uint8_t value_byte = found == memory.end() ? 0U : found->second;
            const auto xor_always = read_xor.find(address);
            if (xor_always != read_xor.end()) value_byte ^= xor_always->second;
            if (patches_applied) {
                const auto xor_after = read_xor_after_patches.find(address);
                if (xor_after != read_xor_after_patches.end())
                    value_byte ^= xor_after->second;
            }
            data[byte] = value_byte;
        }
        return complete(length);
    }

    usb::TransferOutcome control_out(std::uint8_t request_type,
                                     std::uint8_t request,
                                     std::uint16_t value,
                                     std::uint16_t index,
                                     std::uint16_t length,
                                     const std::uint8_t* data,
                                     std::uint32_t) noexcept override
    {
        Request record;
        record.in = false;
        record.address = ((static_cast<std::uint32_t>(value) & 0xFFU) << 24U) |
                         ((static_cast<std::uint32_t>(value) & 0xFF00U) << 8U) |
                         index;
        record.length = length;
        if (data != nullptr) {
            for (std::uint16_t byte = 0U; byte < length && byte < record.data.size();
                 ++byte)
                record.data[byte] = data[byte];
        }
        requests.push_back(record);
        ++out_calls;
        if (request_type != 0x40U || request != 0xBCU || data == nullptr ||
            length < 4U || length > 64U || (length % 4U) != 0U ||
            (record.address & 0x03U) != 0U ||
            (!patches_applied &&
             (length != 4U || (record.address >> 24U) != 0U)))
            return failure(usb::TransferState::failed, usb::TransferCause::invalid);
        if (out_hook) {
            const auto injected = out_hook(record, out_calls);
            if (injected) return *injected;
        }
        for (std::uint16_t byte = 0U; byte < length; ++byte) {
            if (store_only_first_word_of_block && byte >= 4U) continue;
            memory[record.address + byte] = data[byte];
        }
        if (record.address == kPatchAddresses[4])
            patches_applied = true;
        return complete(length);
    }

    usb::TransferOutcome bulk_in(std::uint8_t, std::uint8_t*, std::size_t,
                                 std::uint32_t) noexcept override
    {
        return failure(usb::TransferState::failed, usb::TransferCause::invalid);
    }

    usb::TransferOutcome bulk_out(std::uint8_t, const std::uint8_t*, std::size_t,
                                  std::uint32_t) noexcept override
    {
        return failure(usb::TransferState::failed, usb::TransferCause::invalid);
    }

    void release_interface(std::uint8_t) noexcept override {}
    void close() noexcept override {}
};

struct Harness final {
    FakeDevice* device = nullptr;
    std::unique_ptr<usb::Transport> transport;
    std::unique_ptr<w::Bridge> bridge;

    ~Harness() = default;
    Harness(Harness&&) = default;
    Harness& operator=(Harness&&) = default;
    Harness() = default;
};

Harness make_harness()
{
    auto fake = std::unique_ptr<FakeDevice>(new FakeDevice());
    FakeDevice* device = fake.get();
    usb::OpenResult opened = usb::Transport::open(std::move(fake), location());
    CHECK(opened.outcome.state == usb::OpenState::ok);
    CHECK(opened.transport != nullptr);
    Harness harness;
    harness.device = device;
    harness.transport = std::move(opened.transport);
    if (harness.transport)
        harness.bridge.reset(new w::Bridge(*harness.transport));
    return harness;
}

w::RegisteredImage bank0_record()
{
    w::RegisteredImage image;
    image.format = w::ImageFormat::twdn;
    image.bank_base = TORNE_WIDEN_BANK0_BASE;
    image.image_bytes = static_cast<std::uint32_t>(sizeof(torne_widen_bank0_bytes));
    image.entry_read = TORNE_WIDEN_BANK0_ENTRY_READ;
    image.entry_write = TORNE_WIDEN_BANK0_ENTRY_WRITE;
    image.expected_image = torne_widen_bank0_bytes;
    return image;
}

w::RegisteredImage bank1_record()
{
    w::RegisteredImage image;
    image.format = w::ImageFormat::twdn;
    image.bank_base = TORNE_WIDEN_BANK1_BASE;
    image.image_bytes = static_cast<std::uint32_t>(sizeof(torne_widen_bank1_bytes));
    image.entry_read = TORNE_WIDEN_BANK1_ENTRY_READ;
    image.entry_write = TORNE_WIDEN_BANK1_ENTRY_WRITE;
    image.expected_image = torne_widen_bank1_bytes;
    return image;
}

std::array<std::uint32_t, 5U> five_for(const w::RegisteredImage& image)
{
    return {w::make_jump_word(image.entry_read), 0x970C9FA0U,
            w::make_jump_word(image.entry_write), 0x970C9FA0U,
            0x9FA09F8DU};
}

void load_five(FakeDevice& device, const std::array<std::uint32_t, 5U>& five)
{
    for (std::size_t index = 0U; index < five.size(); ++index)
        device.load_word(kPatchAddresses[index], five[index]);
}

void load_pristine(FakeDevice& device)
{
    load_five(device, kPristineWords);
}

void load_bank_image(FakeDevice& device, const w::RegisteredImage& image)
{
    device.load_bytes(image.bank_base, image.expected_image, image.image_bytes);
}

void load_known_bank0(FakeDevice& device)
{
    const w::RegisteredImage image = bank0_record();
    load_bank_image(device, image);
    load_five(device, five_for(image));
    device.patches_applied = true;
}

void load_own_bank0(FakeDevice& device)
{
    load_known_bank0(device);
}

w::IntroductionPolicy allowed_policy()
{
    w::IntroductionPolicy policy;
    policy.allow_bank_write = true;
    policy.allow_functional_check = true;
    return policy;
}

void test_pristine_bank0_success()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(result.ok);
    CHECK(result.observed == w::WidenState::pristine);
    CHECK(result.target_bank_base == kBank0);
    CHECK(result.image_written);
    CHECK(result.patches_applied);
    CHECK(result.functional_checked);
    CHECK(result.legacy_24bit_checked);
    CHECK(device.writes_in(kBank1, 0x0E2000U) == 0U);
    for (std::uint32_t offset = 0U; offset < bank0.image_bytes; offset += 4U) {
        CHECK(device.word(kBank0 + offset) ==
              w::decode_be32({bank0.expected_image[offset],
                              bank0.expected_image[offset + 1U],
                              bank0.expected_image[offset + 2U],
                              bank0.expected_image[offset + 3U]}));
    }
    const auto five = five_for(bank0);
    for (std::size_t index = 0U; index < five.size(); ++index)
        CHECK(device.word(kPatchAddresses[index]) == five[index]);
    std::array<std::uint32_t, 5U> patch_order{};
    std::size_t patch_writes = 0U;
    for (const Request& request : device.requests) {
        if (request.in) continue;
        for (const std::uint32_t address : kPatchAddresses) {
            if (request.address == address) {
                CHECK(patch_writes < patch_order.size());
                if (patch_writes < patch_order.size())
                    patch_order[patch_writes] = request.address;
                ++patch_writes;
            }
        }
    }
    CHECK(patch_writes == 5U);
    for (std::size_t index = 0U; index < patch_order.size(); ++index)
        CHECK(patch_order[index] == kPatchAddresses[index]);
    bool patches_applied = false;
    for (const Request& request : device.requests) {
        if (request.in) continue;
        if (!patches_applied)
            CHECK(request.length == 4U && (request.address >> 24U) == 0U);
        if (request.address == kPatchAddresses[4]) patches_applied = true;
    }
    CHECK(device.writes_in(kProbe, kProbe + 64U) == 1U);
}

void test_foreign_known_uses_other_bank()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_known_bank0(device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const std::array<w::RegisteredImage, 1U> known{bank0};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank1, known.data(), known.size(), images.data(),
        images.size(), allowed_policy());
    CHECK(result.ok);
    CHECK(result.observed == w::WidenState::foreign_known);
    CHECK(result.target_bank_base == kBank1);
    CHECK(device.writes_in(kBank0, 0x0E1800U) == 0U);
    for (std::uint32_t offset = 0U; offset < bank1.image_bytes; offset += 4U) {
        CHECK(device.word(kBank1 + offset) ==
              w::decode_be32({bank1.expected_image[offset],
                              bank1.expected_image[offset + 1U],
                              bank1.expected_image[offset + 2U],
                              bank1.expected_image[offset + 3U]}));
    }
}

void test_closed_policy_writes_nothing()
{
    Harness pristine = make_harness();
    load_pristine(*pristine.device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult closed = w::introduce_widen(
        *pristine.bridge, &bank0, nullptr, 0U, images.data(), images.size());
    CHECK(!closed.ok);
    CHECK(closed.error == w::LoaderError::introduction_not_allowed);
    CHECK(!closed.wrote_any);
    CHECK(pristine.device->writes_in(0U, 0xFFFFFFFFU) == 0U);

    Harness foreign = make_harness();
    load_known_bank0(*foreign.device);
    const std::array<w::RegisteredImage, 1U> known{bank0};
    const w::LoaderResult known_closed = w::introduce_widen(
        *foreign.bridge, &bank1, known.data(), known.size(), images.data(),
        images.size());
    CHECK(!known_closed.ok);
    CHECK(known_closed.error == w::LoaderError::introduction_not_allowed);
    CHECK(foreign.device->writes_in(0U, 0xFFFFFFFFU) == 0U);
}

void test_ours_active_skips_reload()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_own_bank0(device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(result.ok);
    CHECK(result.observed == w::WidenState::ours_active);
    CHECK(result.target_bank_base == kBank0);
    CHECK(!result.image_written);
    CHECK(result.patches_applied);
    CHECK(result.functional_checked);
    CHECK(device.writes_in(kBank0, 0x0E2000U) == 0U);
    CHECK(device.writes_in(kProbe, kProbe + 64U) == 1U);
}

void test_ours_requires_functional_check()
{
    Harness harness = make_harness();
    load_own_bank0(*harness.device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::functional_check_not_allowed);
    CHECK(!result.image_written);
    CHECK(harness.device->writes_in(0U, 0xFFFFFFFFU) == 0U);
}

void test_unknown_and_read_failure_write_nothing()
{
    Harness unknown = make_harness();
    load_five(*unknown.device, {1U, 2U, 3U, 4U, 5U});
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult unknown_result = w::introduce_widen(
        *unknown.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!unknown_result.ok);
    CHECK(unknown_result.error == w::LoaderError::unknown_state);
    CHECK(unknown.device->writes_in(0U, 0xFFFFFFFFU) == 0U);

    Harness broken = make_harness();
    load_pristine(*broken.device);
    int reads = 0;
    broken.device->in_hook = [&](const Request& request, int) {
        if (request.length == 4U && (request.address == kPatchAddresses[0] ||
                                     request.address == kPatchAddresses[1])) {
            ++reads;
            if (reads <= 2)
                return std::optional<usb::TransferOutcome>(failure(
                    usb::TransferState::failed, usb::TransferCause::timeout));
        }
        return std::optional<usb::TransferOutcome>();
    };
    const w::LoaderResult broken_result = w::introduce_widen(
        *broken.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!broken_result.ok);
    CHECK(broken_result.error == w::LoaderError::read_failed);
    CHECK(broken.device->writes_in(0U, 0xFFFFFFFFU) == 0U);
    CHECK(broken.device->in_calls == 2);
}

void test_state_change_after_recognition()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    int reads = 0;
    device.in_hook = [&](const Request& request, int) {
        if (request.address >= kPatchAddresses[0] &&
            request.address <= kPatchAddresses[4]) {
            ++reads;
            if (reads == 5)
                device.load_word(kPatchAddresses[2], 0x12345678U);
        }
        return std::optional<usb::TransferOutcome>();
    };
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::state_changed);
    CHECK(!result.wrote_any);
    CHECK(device.writes_in(0U, 0xFFFFFFFFU) == 0U);
}

void test_image_write_failure_stops_before_patches()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    bool injected = false;
    device.out_hook = [&](const Request& request, int) {
        if (!injected && request.address == kBank0 + 0x20U) {
            injected = true;
            return std::optional<usb::TransferOutcome>(failure(
                usb::TransferState::failed, usb::TransferCause::no_device));
        }
        return std::optional<usb::TransferOutcome>();
    };
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::device_gone);
    CHECK(result.wrote_any);
    CHECK(!result.patches_applied);
    CHECK(device.writes_in(kPatchAddresses[0], kPatchAddresses[4] + 4U) == 0U);
    CHECK(device.writes_in(kProbe, kProbe + 64U) == 0U);
}

void test_image_readback_mismatch_stops()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    device.read_xor[kBank0 + 0x40U] = 0xFFU;
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::readback_mismatch);
    CHECK(result.image_written);
    CHECK(!result.patches_applied);
    CHECK(device.writes_in(kPatchAddresses[0], kPatchAddresses[4] + 4U) == 0U);
}

void test_patch_readback_mismatch_stops_functional()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    device.read_xor_after_patches[kPatchAddresses[2]] = 0x01U;
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::readback_mismatch);
    CHECK(result.patches_applied);
    CHECK(!result.functional_checked);
    CHECK(device.writes_in(kProbe, kProbe + 64U) == 0U);
    CHECK(device.writes_in(0x01600000U, 0x01700004U) == 0U);
}

void test_image_readback_stage_can_stop_before_patches()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    w::IntroductionPolicy policy = allowed_policy();
    policy.stop_after_image_readback = true;
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        policy);
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::image_verified_stop);
    CHECK(result.image_written);
    CHECK(!result.patches_applied);
    CHECK(device.writes_in(kPatchAddresses[0], kPatchAddresses[4] + 4U) == 0U);
    CHECK(device.writes_in(kProbe, kProbe + 64U) == 0U);
}

void test_bank_write_without_functional_permission_stops()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    w::IntroductionPolicy policy;
    policy.allow_bank_write = true;
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        policy);
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::functional_check_not_allowed);
    CHECK(result.image_written);
    CHECK(result.patches_applied);
    CHECK(!result.functional_checked);
    CHECK(device.writes_in(kProbe, kProbe + 64U) == 0U);
    CHECK(device.writes_in(0x01600000U, 0x01700004U) == 0U);
}

void test_functional_probe_mismatch()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    device.read_xor_after_patches[kProbe] = 0xFFU;
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::functional_check_failed);
    CHECK(result.patches_applied);
    CHECK(!result.functional_checked);
}

void test_read_timeout_retries_once()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    bool injected = false;
    device.in_hook = [&](const Request& request, int) {
        if (!injected && request.address == kPatchAddresses[0]) {
            injected = true;
            return std::optional<usb::TransferOutcome>(failure(
                usb::TransferState::failed, usb::TransferCause::timeout));
        }
        return std::optional<usb::TransferOutcome>();
    };
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(result.ok);
    CHECK(device.in_calls >= 6);
}

void test_missing_or_invalid_image_rejected()
{
    Harness missing = make_harness();
    load_pristine(*missing.device);
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 1U> only_bank1{bank1};
    const w::LoaderResult missing_result = w::introduce_widen(
        *missing.bridge, &bank1, nullptr, 0U, only_bank1.data(),
        only_bank1.size(), allowed_policy());
    CHECK(!missing_result.ok);
    CHECK(missing_result.error == w::LoaderError::missing_image);
    CHECK(!missing_result.wrote_any);
    CHECK(missing.device->writes_in(0U, 0xFFFFFFFFU) == 0U);

    Harness invalid = make_harness();
    load_pristine(*invalid.device);
    const w::RegisteredImage bank0 = bank0_record();
    std::array<std::uint8_t, sizeof(torne_widen_bank0_bytes)> bad_bytes{};
    std::memcpy(bad_bytes.data(), bank0.expected_image, bad_bytes.size());
    bad_bytes[0] = 0x00U;
    w::RegisteredImage bad = bank0;
    bad.expected_image = bad_bytes.data();
    const std::array<w::RegisteredImage, 2U> images{bad, bank1};
    const w::LoaderResult invalid_result = w::introduce_widen(
        *invalid.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!invalid_result.ok);
    CHECK(invalid_result.error == w::LoaderError::invalid_argument);
    CHECK(invalid.device->writes_in(0U, 0xFFFFFFFFU) == 0U);
}

void test_short_readback_stops_before_patches()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    int attempts = 0;
    device.in_hook = [&](const Request& request, int) {
        if (request.address == kBank0 + 0x40U) {
            ++attempts;
            return std::optional<usb::TransferOutcome>(failure(
                usb::TransferState::short_transfer, usb::TransferCause::none, 2U));
        }
        return std::optional<usb::TransferOutcome>();
    };
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::read_failed);
    CHECK(result.image_written);
    CHECK(!result.patches_applied);
    CHECK(attempts == 2);
    CHECK(device.writes_in(kPatchAddresses[0], kPatchAddresses[4] + 4U) == 0U);
}

void test_outcome_unknown_write_keeps_no_progress()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    int attempts = 0;
    device.out_hook = [&](const Request& request, int) {
        if (request.address == kBank0 + 0x40U && attempts < 2) {
            ++attempts;
            return std::optional<usb::TransferOutcome>(failure(
                usb::TransferState::outcome_unknown, usb::TransferCause::timeout));
        }
        return std::optional<usb::TransferOutcome>();
    };
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::outcome_unknown);
    CHECK(result.wrote_any);
    CHECK(!result.patches_applied);
    CHECK(device.writes_in(kPatchAddresses[0], kPatchAddresses[4] + 4U) == 0U);
}

void test_foreign_state_change_uses_recognized_identity()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_known_bank0(device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> known{bank0, bank1};
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    int patch_reads = 0;
    device.in_hook = [&](const Request& request, int) {
        for (const std::uint32_t address : kPatchAddresses) {
            if (request.address == address) {
                ++patch_reads;
                if (patch_reads == 5)
                    load_five(device, five_for(bank1));
            }
        }
        return std::optional<usb::TransferOutcome>();
    };
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank1, known.data(), known.size(), images.data(),
        images.size(), allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::state_changed);
    CHECK(!result.wrote_any);
    CHECK(device.writes_in(0U, 0xFFFFFFFFU) == 0U);
}

void test_foreign_same_five_different_builds_keeps_match()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_known_bank0(device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    std::array<std::uint8_t, sizeof(torne_widen_bank0_bytes)> variant_bytes{};
    std::memcpy(variant_bytes.data(), bank0.expected_image, variant_bytes.size());
    variant_bytes[0x20U] ^= 0xFFU;
    w::RegisteredImage variant = bank0;
    variant.expected_image = variant_bytes.data();
    const std::array<w::RegisteredImage, 2U> known{bank0, variant};
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank1, known.data(), known.size(), images.data(),
        images.size(), allowed_policy());
    CHECK(result.ok);
    CHECK(result.observed == w::WidenState::foreign_known);
    CHECK(result.target_bank_base == kBank1);
}

void test_duplicate_same_image_stays_unknown()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_known_bank0(device);
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> known{bank0, bank0};
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank1, known.data(), known.size(), images.data(),
        images.size(), allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::unknown_state);
    CHECK(device.writes_in(0U, 0xFFFFFFFFU) == 0U);
}

void test_invalid_functional_policy_rejected()
{
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};

    Harness short_probe = make_harness();
    load_own_bank0(*short_probe.device);
    w::IntroductionPolicy short_policy = allowed_policy();
    short_policy.functional_probe_length = 4U;
    const w::LoaderResult short_result = w::introduce_widen(
        *short_probe.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        short_policy);
    CHECK(!short_result.ok);
    CHECK(short_result.error == w::LoaderError::invalid_argument);
    CHECK(short_probe.device->writes_in(0U, 0xFFFFFFFFU) == 0U);

    Harness duplicate_hold = make_harness();
    load_own_bank0(*duplicate_hold.device);
    w::IntroductionPolicy duplicate_policy = allowed_policy();
    duplicate_policy.core1_hold_address = duplicate_policy.core0_hold_address;
    const w::LoaderResult duplicate_result = w::introduce_widen(
        *duplicate_hold.bridge, &bank0, nullptr, 0U, images.data(),
        images.size(), duplicate_policy);
    CHECK(!duplicate_result.ok);
    CHECK(duplicate_result.error == w::LoaderError::invalid_argument);
    CHECK(duplicate_hold.device->writes_in(0U, 0xFFFFFFFFU) == 0U);
}

void test_multiword_write_truncation_is_detected()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    load_pristine(device);
    device.store_only_first_word_of_block = true;
    const w::RegisteredImage bank0 = bank0_record();
    const w::RegisteredImage bank1 = bank1_record();
    const std::array<w::RegisteredImage, 2U> images{bank0, bank1};
    const w::LoaderResult result = w::introduce_widen(
        *harness.bridge, &bank0, nullptr, 0U, images.data(), images.size(),
        allowed_policy());
    CHECK(!result.ok);
    CHECK(result.error == w::LoaderError::functional_check_failed);
    CHECK(result.patches_applied);
    CHECK(!result.functional_checked);
}

void test_bridge_boundaries_and_be()
{
    Harness harness = make_harness();
    FakeDevice& device = *harness.device;
    device.patches_applied = true;
    device.load_word(0x00FFFFFCU, 0x11223344U);
    const w::ReadResult narrow = harness.bridge->read32(0x00FFFFFCU);
    CHECK(narrow.ok);
    CHECK(narrow.value == 0x11223344U);
    CHECK(!harness.bridge->read32(0x01000000U).ok);
    const w::ReadResult wide = harness.bridge->read32_wide(0x01000000U);
    CHECK(wide.ok);
    CHECK(harness.bridge->write32(0x01000000U, 0x01020304U, true).ok);
    CHECK(device.word(0x01000000U) == 0x01020304U);
    CHECK(!harness.bridge->write32(0x01000000U, 0x01020304U, false).ok);
    CHECK(!harness.bridge->write_block_wide(0x01000000U, nullptr, 4U).ok);
    std::array<std::uint8_t, 64U> block{};
    CHECK(harness.bridge->write_block_wide(kProbe, block.data(), block.size()).ok);
    CHECK(harness.bridge->read_block_wide(kProbe, block.size()).ok);
    CHECK(!harness.bridge->read_block_wide(kProbe, 68U).ok);
    CHECK(!harness.bridge->read_block_wide(kProbe + 2U, 4U).ok);
    CHECK(harness.bridge->read32(0x00FFFFFCU).ok);
    CHECK(harness.bridge->write_block_wide(0x01FFFFC0U, block.data(),
                                           block.size()).ok);
    CHECK(harness.bridge->read_block_wide(0x01FFFFC0U, block.size()).ok);

    const int in_before = device.in_calls;
    const int out_before = device.out_calls;
    CHECK(!harness.bridge->read32_wide(0xFE000000U).ok);
    CHECK(!harness.bridge->read32_wide(0x02000000U).ok);
    CHECK(!harness.bridge->read32_wide(0x01FFFFFFU).ok);
    CHECK(!harness.bridge->read_block_wide(0x01FFFFFCU, block.size()).ok);
    CHECK(!harness.bridge->write_block_wide(0x01FFFFFCU, block.data(),
                                            block.size()).ok);
    CHECK(!harness.bridge->write_block_wide(0xFFFFFFFCU, block.data(),
                                            block.size()).ok);
    CHECK(!harness.bridge->write32(0xFE000000U, 0U, true).ok);
    CHECK(device.in_calls == in_before);
    CHECK(device.out_calls == out_before);
}

}  // namespace

int main()
{
    test_pristine_bank0_success();
    test_foreign_known_uses_other_bank();
    test_closed_policy_writes_nothing();
    test_ours_active_skips_reload();
    test_ours_requires_functional_check();
    test_unknown_and_read_failure_write_nothing();
    test_state_change_after_recognition();
    test_image_write_failure_stops_before_patches();
    test_image_readback_mismatch_stops();
    test_patch_readback_mismatch_stops_functional();
    test_image_readback_stage_can_stop_before_patches();
    test_bank_write_without_functional_permission_stops();
    test_functional_probe_mismatch();
    test_read_timeout_retries_once();
    test_missing_or_invalid_image_rejected();
    test_short_readback_stops_before_patches();
    test_outcome_unknown_write_keeps_no_progress();
    test_foreign_state_change_uses_recognized_identity();
    test_foreign_same_five_different_builds_keeps_match();
    test_duplicate_same_image_stays_unknown();
    test_invalid_functional_policy_rejected();
    test_multiword_write_truncation_is_detected();
    test_bridge_boundaries_and_be();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("widen loader tests passed\n");
    return 0;
}
