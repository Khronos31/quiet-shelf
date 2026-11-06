// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/boot.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include <dtv/internal/clock.hpp>
#include <dtv/internal/result.hpp>

namespace dtv::internal {

namespace {

constexpr dtv_internal_result make_result(int32_t code, uint32_t detail = 0);
bool succeeded(dtv_internal_result result);

class UsbBootOwner final : public BootUsbOwner {
public:
    explicit UsbBootOwner(std::unique_ptr<UsbDevice> device) : device_(std::move(device)) {}
    const dtv_usb_io *io() const override { return device_ ? device_->io() : nullptr; }

private:
    std::unique_ptr<UsbDevice> device_;
};

class UsbBootProvider final : public BootUsbProvider {
public:
    explicit UsbBootProvider(const UsbContext &usb) : usb_(usb) {}
    std::unique_ptr<BootUsbOwner> open(const UsbDeviceSnapshot &candidate, int interface_number,
                                       const dtv_runtime_clock *clock,
                                       dtv_internal_result *result) override
    {
        auto device = usb_.open(candidate, interface_number, clock);
        if (!device) {
            *result = make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
            return nullptr;
        }
        *result = device->result();
        if (!succeeded(*result)) {
            return nullptr;
        }
        return std::make_unique<UsbBootOwner>(std::move(device));
    }

private:
    const UsbContext &usb_;
};

constexpr dtv_internal_result make_result(int32_t code, uint32_t detail)
{
    return {code, detail};
}

bool succeeded(dtv_internal_result result)
{
    return result.code == DTV_INTERNAL_RESULT_OK;
}

uint64_t deadline_after(uint64_t now, uint64_t timeout_ms)
{
    if (timeout_ms == UINT64_MAX || timeout_ms > (UINT64_MAX - now) / 1000000) {
        return UINT64_MAX;
    }
    return now + timeout_ms * 1000000;
}

dtv_internal_result remaining_timeout(const dtv_runtime_clock &clock,
                                      uint64_t deadline, dtv_timeout *timeout)
{
    if (timeout == nullptr || clock.monotonic_now_ns == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    }
    if (deadline == UINT64_MAX) {
        timeout->timeout_ms = UINT64_MAX;
        timeout->operation_deadline_ns = UINT64_MAX;
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    uint64_t now = 0;
    const auto result = call_callback([&] {
        now = clock.monotonic_now_ns(clock.user);
        return make_result(DTV_INTERNAL_RESULT_OK);
    });
    if (!succeeded(result)) {
        return result;
    }
    if (now >= deadline) {
        return make_result(DTV_INTERNAL_RESULT_TIMEOUT);
    }
    const auto ns = deadline - now;
    timeout->timeout_ms = ns / 1000000 + (ns % 1000000 != 0 ? 1 : 0);
    timeout->operation_deadline_ns = deadline;
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result cancelled(const dtv_runtime_clock &clock,
                              const dtv_cancel *cancel)
{
    if (cancel == nullptr || clock.is_cancelled == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_OK);
    }
    uint32_t value = 0;
    const auto result = call_callback([&] {
        return clock.is_cancelled(clock.user, cancel, &value);
    });
    if (!succeeded(result)) {
        return result;
    }
    return value != 0 ? make_result(DTV_INTERNAL_RESULT_TIMEOUT) : make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result identify(const spi::DeviceOps &ops, dtv_context *context,
                             const dtv_runtime_clock &clock, uint64_t deadline,
                             dtv_identify_info *info)
{
    if (ops.identify == nullptr || info == nullptr) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    }
    dtv_timeout timeout{};
    auto result = remaining_timeout(clock, deadline, &timeout);
    if (!succeeded(result)) {
        return result;
    }
    info->firmware_key = 0;
    info->state = DTV_IDENTIFY_UNKNOWN;
    result = call_callback([&] { return ops.identify(context, info, timeout); });
    if (!succeeded(result)) {
        return result;
    }
    if (info->state != DTV_IDENTIFY_LOADER && info->state != DTV_IDENTIFY_RUNTIME) {
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
    }
    return make_result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result preflight_result(FirmwarePreflightResult result)
{
    switch (result.error) {
    case FirmwarePreflightError::None:
        return make_result(DTV_INTERNAL_RESULT_OK);
    case FirmwarePreflightError::ResourceExhausted:
        return make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED,
                           static_cast<uint32_t>(result.image_index));
    case FirmwarePreflightError::InvalidArgument:
    case FirmwarePreflightError::InvalidHash:
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT,
                           static_cast<uint32_t>(result.image_index));
    default:
        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED,
                           static_cast<uint32_t>(result.image_index));
    }
}

}

const spi::FirmwareEntry *find_firmware_entry(const spi::ModelEntry &model,
                                               dtv_fw_key key) noexcept
{
    if (model.firmware == nullptr || model.firmware_count == 0) {
        return nullptr;
    }
    for (std::size_t index = 0; index < model.firmware_count; ++index) {
        const auto &entry = model.firmware[index];
        if (entry.key == key && entry.ops != nullptr) {
            return &entry;
        }
    }
    return nullptr;
}

BootSession::~BootSession()
{
    cleanup();
}

void BootSession::cleanup() noexcept
{
    const auto *close_ops = ops_ != nullptr && ops_->close != nullptr ? ops_ : opened_ops_;
    if (open_attempted_ && close_ops != nullptr && close_ops->close != nullptr && instance_) {
        call_callback([&] {
            return close_ops->close(instance_->context(), {0, 0});
        });
    }
    open_attempted_ = false;
    instance_.reset();
    usb_.reset();
}

std::unique_ptr<BootSession> boot_selected_candidate(
    const UsbContext &usb, const BootRequest &request, dtv_internal_result *result)
{
    if (result == nullptr) {
        return nullptr;
    }
    *result = make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    try {
        if (request.selected_candidate == nullptr || request.model == nullptr ||
            request.interface_number < 0 || request.model->bootstrap_ops == nullptr ||
            request.model->model.vendor_id != request.selected_candidate->vendor_id ||
            request.model->model.product_id != request.selected_candidate->product_id ||
            request.images.empty()) {
            return nullptr;
        }
        const auto &bootstrap = *request.model->bootstrap_ops;
        if (bootstrap.open == nullptr || bootstrap.identify == nullptr) {
            *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            return nullptr;
        }
        for (const auto &stage : request.stages) {
            if (stage.image_index >= request.images.size()) {
                return nullptr;
            }
        }

        const auto clock = request.clock != nullptr ? *request.clock : *system_runtime_clock();
        if (clock.monotonic_now_ns == nullptr) {
            *result = make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
            return nullptr;
        }
        uint64_t start = 0;
        auto operation_result = call_callback([&] {
            start = clock.monotonic_now_ns(clock.user);
            return make_result(DTV_INTERNAL_RESULT_OK);
        });
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }
        const auto deadline = deadline_after(start, request.timeout.timeout_ms);
        operation_result = cancelled(clock, request.cancel);
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }
        dtv_timeout phase_timeout{};
        operation_result = remaining_timeout(clock, deadline, &phase_timeout);
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }

        VerifiedFirmwareBundle bundle;
        const auto preflight = load_firmware_bundle(request.images, &bundle);
        *result = preflight_result(preflight);
        if (!succeeded(*result)) {
            return nullptr;
        }
        for (const auto &stage : request.stages) {
            if (stage.image_index >= bundle.size()) {
                *result = make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
                return nullptr;
            }
        }
        operation_result = remaining_timeout(clock, deadline, &phase_timeout);
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }

        auto session = std::unique_ptr<BootSession>(new BootSession);
        session->clock_ = clock;
        operation_result = cancelled(session->clock_, request.cancel);
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }

        UsbBootProvider core_provider(usb);
        auto *provider = request.provider != nullptr ? request.provider : &core_provider;
        operation_result = remaining_timeout(session->clock_, deadline, &phase_timeout);
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }
        session->usb_ = provider->open(*request.selected_candidate, request.interface_number,
                                       &session->clock_, result);
        if (!session->usb_) {
            if (succeeded(*result)) {
                *result = make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
            }
            return nullptr;
        }
        operation_result = cancelled(session->clock_, request.cancel);
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }
        if (!succeeded(*result) || session->usb_->io() == nullptr) {
            if (succeeded(*result)) {
                *result = make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
            }
            return nullptr;
        }

        session->instance_ = std::make_unique<spi::ProductInstance>(*request.model,
                                                                    request.device_id);
        operation_result = session->instance_->result();
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }

        dtv_timeout open_timeout{};
        operation_result = remaining_timeout(session->clock_, deadline, &open_timeout);
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }
        dtv_open_args args{&request.model->model, request.function, open_timeout};
        session->opened_ops_ = &bootstrap;
        session->open_attempted_ = true;
        operation_result = call_callback([&] {
            return bootstrap.open(session->instance_->context(), session->usb_->io(), &args);
        });
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }

        dtv_identify_info info{};
        operation_result = identify(bootstrap, session->instance_->context(),
                                    session->clock_, deadline, &info);
        if (!succeeded(operation_result)) {
            *result = operation_result;
            return nullptr;
        }
        if (info.state == DTV_IDENTIFY_LOADER) {
            if (request.stages.empty() || bootstrap.firmware == nullptr) {
                *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
                return nullptr;
            }
            for (const auto &stage : request.stages) {
                operation_result = cancelled(session->clock_, request.cancel);
                if (!succeeded(operation_result)) {
                    *result = operation_result;
                    return nullptr;
                }
                dtv_timeout timeout{};
                operation_result = remaining_timeout(session->clock_, deadline, &timeout);
                if (!succeeded(operation_result)) {
                    *result = operation_result;
                    return nullptr;
                }
                const auto image = bundle.image(stage.image_index);
                dtv_bytes bytes{image.data(), image.size()};
                dtv_firmware_result firmware_result{};
                firmware_result.transition = UINT32_MAX;
                operation_result = call_callback([&] {
                    return bootstrap.firmware(session->instance_->context(), stage.stage,
                                              bytes, &firmware_result, timeout);
                });
                if (!succeeded(operation_result)) {
                    *result = operation_result;
                    return nullptr;
                }
                if (firmware_result.transition != DTV_FIRMWARE_SAME_HANDLE) {
                    *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
                    return nullptr;
                }
            }
            operation_result = identify(bootstrap, session->instance_->context(),
                                        session->clock_, deadline, &info);
            if (!succeeded(operation_result)) {
                *result = operation_result;
                return nullptr;
            }
        }
        if (info.state != DTV_IDENTIFY_RUNTIME) {
            *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            return nullptr;
        }
        const auto *entry = find_firmware_entry(*request.model, info.firmware_key);
        if (entry == nullptr) {
            *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            return nullptr;
        }
        session->ops_ = entry->ops;
        *result = make_result(DTV_INTERNAL_RESULT_OK);
        return session;
    } catch (...) {
        *result = exception_result();
        return nullptr;
    }
}

}
