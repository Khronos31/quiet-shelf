// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/boot.hpp>
#include <dtv/internal/clock.hpp>
#include <dtv/internal/result.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {

struct State {
    int value;
};

int destroyed = 0;
const dtv_usb_io *opened_io = nullptr;
const dtv_usb_io fake_io{};
int identify_calls = 0;
int firmware_calls = 0;
int bootstrap_open_calls = 0;
int bootstrap_close_calls = 0;
int product_destroy_calls = 0;
int owner_destroy_calls = 0;
int cleanup_order = 0;
int close_order = 0;
int destroy_order = 0;
int owner_order = 0;
dtv_internal_result bootstrap_open_result{DTV_INTERNAL_RESULT_OK, 0};
uint32_t fake_transition = DTV_FIRMWARE_SAME_HANDLE;
bool throw_firmware = false;
uint32_t final_identify_state = DTV_IDENTIFY_RUNTIME;
dtv_fw_key final_firmware_key = 9;
uint64_t fake_deadline = 0;
uint64_t open_timeout_ms = 0;
uint64_t identify_timeout_ms[2]{};
uint64_t firmware_timeout_ms = 0;

struct FakeClockState {
    uint64_t now_ns;
    uint32_t cancelled;
};

struct FakeOwner final : dtv::internal::BootUsbOwner {
    ~FakeOwner() override
    {
        ++owner_destroy_calls;
        owner_order = ++cleanup_order;
    }
    const dtv_usb_io *io() const override { return &fake_io; }
};

struct FakeProvider final : dtv::internal::BootUsbProvider {
    int opens = 0;
    std::unique_ptr<dtv::internal::BootUsbOwner> open(
        const dtv::internal::UsbDeviceSnapshot &, int,
        const dtv_runtime_clock *, dtv_internal_result *result) override
    {
        ++opens;
        *result = {DTV_INTERNAL_RESULT_OK, 0};
        return std::make_unique<FakeOwner>();
    }
};

dtv_internal_result fake_open(dtv_context *, const dtv_usb_io *io,
                              const dtv_open_args *args)
{
    ++bootstrap_open_calls;
    opened_io = io;
    open_timeout_ms = args->timeout.timeout_ms;
    fake_deadline = args->timeout.operation_deadline_ns;
    if (bootstrap_open_result.code != DTV_INTERNAL_RESULT_OK) {
        return bootstrap_open_result;
    }
    return args->timeout.operation_deadline_ns != 0
        ? dtv_internal_result{DTV_INTERNAL_RESULT_OK, 0}
        : dtv_internal_result{DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0};
}

dtv_internal_result fake_create_state(dtv_device_id, dtv_context **context)
{
    *context = new int(1);
    return {DTV_INTERNAL_RESULT_OK, 0};
}

void fake_destroy_state(dtv_context *context)
{
    delete static_cast<int *>(context);
    ++product_destroy_calls;
    destroy_order = ++cleanup_order;
}

dtv_internal_result fake_close(dtv_context *, dtv_timeout)
{
    ++bootstrap_close_calls;
    close_order = ++cleanup_order;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

dtv_internal_result fake_identify(dtv_context *, dtv_identify_info *info, dtv_timeout timeout)
{
    if (identify_calls < 2) {
        identify_timeout_ms[identify_calls] = timeout.timeout_ms;
    }
    ++identify_calls;
    info->state = identify_calls == 1 ? DTV_IDENTIFY_LOADER : final_identify_state;
    info->firmware_key = final_firmware_key;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

dtv_internal_result fake_firmware(dtv_context *, uint32_t stage, dtv_bytes bytes,
                                  dtv_firmware_result *result, dtv_timeout timeout)
{
    ++firmware_calls;
    firmware_timeout_ms = timeout.timeout_ms;
    if (stage != 42 || bytes.size != 3 || bytes.data[0] != 'a' ||
        bytes.data[1] != 'b' || bytes.data[2] != 'c' ||
        timeout.operation_deadline_ns == 0) {
        return {DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0};
    }
    if (throw_firmware) {
        throw std::runtime_error("fixture callback failure");
    }
    result->transition = fake_transition;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

const dtv::spi::DeviceOps fake_bootstrap{
    .open = fake_open,
    .identify = fake_identify,
    .firmware = fake_firmware,
    .close = fake_close,
};
const dtv::spi::DeviceOps fake_runtime{};
const dtv::spi::FirmwareEntry fake_firmware_table[]{{9, &fake_runtime}};
const dtv_usb_identity fake_cold_identities[]{{0xabcd, 0x1000}};
const dtv::spi::ModelEntry fake_model{
    .model = {"fake", 0x1234, 0x5678},
    .firmware = fake_firmware_table,
    .firmware_count = 1,
    .create_state = fake_create_state,
    .destroy_state = fake_destroy_state,
    .bootstrap_ops = &fake_bootstrap,
    .cold_identities = fake_cold_identities,
    .cold_identity_count = 1,
};

uint64_t fake_clock_now(void *user)
{
    auto *state = static_cast<FakeClockState *>(user);
    state->now_ns += 2000000;
    return state->now_ns;
}

dtv_internal_result fake_clock_cancelled(void *user, const dtv_cancel *, uint32_t *cancelled)
{
    *cancelled = static_cast<FakeClockState *>(user)->cancelled;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

dtv_internal_result fake_clock_delay(void *, uint64_t, uint64_t, const dtv_cancel *, uint32_t *cancelled)
{
    *cancelled = 0;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

dtv_internal_result throwing_create(dtv_device_id, dtv_context **out)
{
    *out = new State{1};
    throw std::bad_alloc{};
}

dtv_internal_result failed_create(dtv_device_id, dtv_context **out)
{
    *out = new State{2};
    return {DTV_INTERNAL_RESULT_UNSUPPORTED, 37};
}

void throwing_destroy(dtv_context *context)
{
    delete static_cast<State *>(context);
    ++destroyed;
    throw 1;
}

const dtv::spi::DeviceOps ops{};
const dtv::spi::FirmwareEntry firmware[]{{9, &ops}};
const dtv::spi::ModelEntry throwing_model{
    .model = {"throwing", 1, 2},
    .firmware = firmware,
    .firmware_count = 1,
    .create_state = throwing_create,
    .destroy_state = throwing_destroy,
};
const dtv::spi::ModelEntry failed_model{
    .model = {"failed", 1, 2},
    .firmware = firmware,
    .firmware_count = 1,
    .create_state = failed_create,
    .destroy_state = throwing_destroy,
};

bool exception_mapping()
{
    const auto oom = dtv::internal::exception_result(std::make_exception_ptr(std::bad_alloc{}));
    const auto other = dtv::internal::exception_result(std::make_exception_ptr(1));
    return oom.code == DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED && oom.detail == 0 &&
           other.code == DTV_INTERNAL_RESULT_INTERNAL && other.detail == 0;
}

bool product_state_cleanup()
{
    destroyed = 0;
    {
        dtv::spi::ProductInstance instance{throwing_model, 1};
        if (instance.result().code != DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED ||
            instance.context() != nullptr || destroyed != 1) {
            return false;
        }
    }
    if (destroyed != 1) {
        return false;
    }
    {
        dtv::spi::ProductInstance instance{failed_model, 2};
        if (instance.result().code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
            instance.result().detail != 37 || instance.context() != nullptr ||
            destroyed != 2) {
            return false;
        }
    }
    return destroyed == 2;
}

bool exact_binding()
{
    return dtv::internal::find_firmware_entry(throwing_model, 9) == &firmware[0] &&
           dtv::internal::find_firmware_entry(throwing_model, 10) == nullptr &&
           dtv::internal::find_firmware_entry(failed_model, 9)->ops == &ops;
}

bool clock_deadline()
{
    const auto *clock = dtv::internal::system_runtime_clock();
    const auto now = clock->monotonic_now_ns(clock->user);
    uint32_t cancelled = 0;
    const auto wake = clock->delay_until(clock->user, now + 1000000,
                                         now + 50000000, nullptr, &cancelled);
    const auto expired = clock->delay_until(clock->user, now, now, nullptr, &cancelled);
    return wake.code == DTV_INTERNAL_RESULT_OK && cancelled == 0 &&
           expired.code == DTV_INTERNAL_RESULT_TIMEOUT;
}

bool boot_rejects_before_open()
{
    const dtv::spi::DeviceOps bootstrap{
        .open = nullptr,
        .identify = nullptr,
    };
    const dtv::spi::ModelEntry model{
        .model = {"unreachable", 0x1234, 0x5678},
        .firmware = firmware,
        .firmware_count = 1,
        .bootstrap_ops = &bootstrap,
        .cold_identities = fake_cold_identities,
        .cold_identity_count = 1,
    };
    const dtv::internal::FirmwareImageSpec image{
        std::filesystem::temp_directory_path() / "dtv11-i1c-missing-firmware.bin",
        1,
        "0000000000000000000000000000000000000000000000000000000000000000",
    };
    const dtv::internal::BootStage stage{1, 0};
    dtv::internal::UsbDeviceSnapshot candidate{};
    candidate.vendor_id = model.model.vendor_id;
    candidate.product_id = model.model.product_id;
    dtv::internal::UsbContext usb;
    dtv_internal_result result{};

    dtv::internal::BootRequest unselected{};
    unselected.model = &model;
    unselected.images = std::span{&image, 1};
    unselected.stages = std::span{&stage, 1};
    if (dtv::internal::boot_selected_candidate(usb, unselected, &result) ||
        result.code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT) {
        return false;
    }

    auto corrupt = unselected;
    corrupt.selected_candidate = &candidate;
    corrupt.interface_number = 0;
    FakeProvider provider;
    corrupt.provider = &provider;
    if (dtv::internal::boot_selected_candidate(usb, corrupt, &result) ||
        result.code != DTV_INTERNAL_RESULT_UNSUPPORTED || result.detail != 0 || provider.opens != 0) {
        return false;
    }
    return true;
}

bool same_handle_boot()
{
    const auto path = std::filesystem::temp_directory_path() / "dtv11-i1c-firmware-abc.bin";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "abc";
        if (!file) {
            return false;
        }
    }
    const dtv::internal::FirmwareImageSpec image{
        path, 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    };
    const dtv::internal::BootStage stage{42, 0};
    dtv::internal::UsbDeviceSnapshot candidate{};
    candidate.vendor_id = fake_cold_identities[0].vendor_id;
    candidate.product_id = fake_cold_identities[0].product_id;
    dtv::internal::UsbContext usb;
    FakeProvider provider;
    dtv::internal::BootRequest request{};
    request.selected_candidate = &candidate;
    request.interface_number = 0;
    request.model = &fake_model;
    request.images = std::span{&image, 1};
    request.stages = std::span{&stage, 1};
    request.timeout = {20, 0};
    request.provider = &provider;
    FakeClockState clock_state{};
    const dtv_runtime_clock clock{
        &clock_state, fake_clock_now, fake_clock_delay, fake_clock_cancelled
    };
    request.clock = &clock;
    opened_io = nullptr;
    identify_calls = 0;
    firmware_calls = 0;
    final_identify_state = DTV_IDENTIFY_RUNTIME;
    final_firmware_key = 9;
    identify_timeout_ms[0] = 0;
    identify_timeout_ms[1] = 0;
    fake_transition = DTV_FIRMWARE_SAME_HANDLE;
    dtv_internal_result result{};
    auto session = dtv::internal::boot_selected_candidate(usb, request, &result);
    const bool same_handle = session && result.code == DTV_INTERNAL_RESULT_OK &&
        provider.opens == 1 && identify_calls == 2 && firmware_calls == 1 &&
        opened_io == session->io() && session->io() == &fake_io &&
        session->ops() == &fake_runtime && open_timeout_ms == 12 &&
        identify_timeout_ms[0] == 10 && firmware_timeout_ms == 8 &&
        identify_timeout_ms[1] == 6 && fake_deadline == 22000000;
    session.reset();

    fake_transition = DTV_FIRMWARE_REENUMERATE;
    provider.opens = 0;
    identify_calls = 0;
    firmware_calls = 0;
    clock_state = {};
    auto rejected = dtv::internal::boot_selected_candidate(usb, request, &result);
    const bool transition_rejected = !rejected && provider.opens == 1 &&
        result.code == DTV_INTERNAL_RESULT_UNSUPPORTED && identify_calls == 1;

    fake_transition = DTV_FIRMWARE_SAME_HANDLE;
    provider.opens = 0;
    identify_calls = 0;
    firmware_calls = 0;
    clock_state = {};
    final_identify_state = DTV_IDENTIFY_UNKNOWN;
    auto unknown_state = dtv::internal::boot_selected_candidate(usb, request, &result);
    const bool unknown_rejected = !unknown_state && result.code == DTV_INTERNAL_RESULT_UNSUPPORTED;

    final_identify_state = DTV_IDENTIFY_RUNTIME;
    final_firmware_key = 10;
    provider.opens = 0;
    identify_calls = 0;
    firmware_calls = 0;
    clock_state = {};
    auto unknown_key = dtv::internal::boot_selected_candidate(usb, request, &result);
    const bool key_rejected = !unknown_key && result.code == DTV_INTERNAL_RESULT_UNSUPPORTED;

    final_firmware_key = 9;
    provider.opens = 0;
    identify_calls = 0;
    firmware_calls = 0;
    clock_state = {};
    throw_firmware = true;
    auto callback_exception = dtv::internal::boot_selected_candidate(usb, request, &result);
    const bool exception_caught = !callback_exception &&
        result.code == DTV_INTERNAL_RESULT_INTERNAL;
    throw_firmware = false;

    provider.opens = 0;
    identify_calls = 0;
    firmware_calls = 0;
    clock_state = {};
    clock_state.cancelled = 1;
    uint8_t cancel_storage = 0;
    request.cancel = reinterpret_cast<const dtv_cancel *>(&cancel_storage);
    auto cancelled = dtv::internal::boot_selected_candidate(usb, request, &result);
    const bool cancel_rejected = !cancelled && provider.opens == 0 &&
        result.code == DTV_INTERNAL_RESULT_TIMEOUT;
    request.cancel = nullptr;

    bootstrap_open_result = {DTV_INTERNAL_RESULT_BUSY, 0x1234};
    provider.opens = 0;
    identify_calls = 0;
    firmware_calls = 0;
    bootstrap_open_calls = 0;
    bootstrap_close_calls = 0;
    product_destroy_calls = 0;
    owner_destroy_calls = 0;
    cleanup_order = 0;
    close_order = 0;
    destroy_order = 0;
    owner_order = 0;
    clock_state = {};
    auto busy = dtv::internal::boot_selected_candidate(usb, request, &result);
    const bool busy_preserved = !busy && provider.opens == 1 && bootstrap_open_calls == 1 &&
        result.code == DTV_INTERNAL_RESULT_BUSY && result.detail == 0x1234 &&
        identify_calls == 0 && firmware_calls == 0 && bootstrap_close_calls == 1 &&
        product_destroy_calls == 1 && owner_destroy_calls == 1 &&
        close_order < destroy_order && destroy_order < owner_order;
    bootstrap_open_result = {DTV_INTERNAL_RESULT_OK, 0};

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    fake_transition = DTV_FIRMWARE_SAME_HANDLE;
    return same_handle && transition_rejected && unknown_rejected && key_rejected && cancel_rejected &&
           busy_preserved;
}

}

int main()
{
    return exception_mapping() && product_state_cleanup() && exact_binding() && clock_deadline() &&
           boot_rejects_before_open() && same_handle_boot()
        ? 0
        : 1;
}
