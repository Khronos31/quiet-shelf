// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/boot.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <string_view>
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

    std::unique_ptr<BootUsbOwner> open_group(
        const UsbDeviceSnapshot &candidate, std::span<const UsbInterfaceClaim> claims,
        const dtv_runtime_clock *clock, dtv_internal_result *result) override
    {
        auto device = usb_.open(candidate, claims, clock);
        if (!device) {
            *result = make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
            return nullptr;
        }
        *result = device->result();
        if (!succeeded(*result)) return nullptr;
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

bool cold_identity_matches(const spi::ModelEntry &model,
                           const UsbDeviceSnapshot &candidate)
{
    if (model.cold_identities == nullptr || model.cold_identity_count == 0) return false;
    for (std::size_t i = 0; i < model.cold_identity_count; ++i) {
        if (model.cold_identities[i].vendor_id == candidate.vendor_id &&
            model.cold_identities[i].product_id == candidate.product_id) return true;
    }
    return false;
}

const spi::FunctionEntry *find_function_entry(const spi::ModelEntry &model, dtv_function function)
{
    if (model.functions == nullptr) return nullptr;
    const spi::FunctionEntry *found = nullptr;
    for (std::size_t i = 0; i < model.function_count; ++i) {
        if (model.functions[i].function == function) {
            if (found != nullptr) return nullptr;
            found = &model.functions[i];
        }
    }
    return found;
}

const spi::FirmwareEntry *find_function_firmware_entry(const spi::FunctionEntry &function,
                                                        dtv_fw_key key)
{
    if (function.firmware == nullptr) return nullptr;
    for (std::size_t i = 0; i < function.firmware_count; ++i) {
        if (function.firmware[i].key == key && function.firmware[i].ops != nullptr) {
            return &function.firmware[i];
        }
    }
    return nullptr;
}

dtv_usb_candidate_info candidate_view(const UsbDeviceSnapshot &snapshot,
                                      std::vector<dtv_usb_interface_info> *interfaces)
{
    interfaces->clear();
    interfaces->reserve(snapshot.interfaces.size());
    for (const auto &source : snapshot.interfaces) {
        auto view = source.info;
        view.endpoints = source.endpoints.data();
        view.endpoint_count = static_cast<std::uint32_t>(source.endpoints.size());
        interfaces->push_back(view);
    }
    return {snapshot.candidate_id, snapshot.vendor_id, snapshot.product_id,
            snapshot.device_version, snapshot.bus_number, snapshot.device_address,
            snapshot.port_path.data(), static_cast<std::uint32_t>(snapshot.port_path.size()),
            interfaces->data(), static_cast<std::uint32_t>(interfaces->size())};
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

dtv_internal_result checkpoint(const dtv_runtime_clock &clock, std::uint64_t deadline,
                                const dtv_cancel *cancel)
{
    auto result = cancelled(clock, cancel);
    if (!succeeded(result)) return result;
    dtv_timeout timeout{};
    return remaining_timeout(clock, deadline, &timeout);
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
    if (info->state != DTV_IDENTIFY_UNKNOWN && info->state != DTV_IDENTIFY_LOADER &&
        info->state != DTV_IDENTIFY_RUNTIME) {
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
            request.images.empty()) {
            return nullptr;
        }
        if (!cold_identity_matches(*request.model, *request.selected_candidate)) {
            *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
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

std::unique_ptr<BootUsbOwner> BootUsbProvider::open_group(
    const UsbDeviceSnapshot &candidate, std::span<const UsbInterfaceClaim> claims,
    const dtv_runtime_clock *clock, dtv_internal_result *result)
{
    if (result == nullptr) return nullptr;
    if (claims.size() != 1 || claims[0].alternate_setting != 0) {
        if (result) *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        return nullptr;
    }
    return open(candidate, claims[0].interface_number, clock, result);
}

struct BootGroupSession::PhysicalOwner {
    std::uint32_t candidate_index;
    std::unique_ptr<BootUsbOwner> owner;
};

struct BootGroupSession::FunctionContext {
    dtv_function function;
    std::uint32_t candidate_index;
    const spi::FunctionEntry *entry;
    const dtv_usb_io *io;
    dtv_context *context = nullptr;
    void (*destroy_state)(dtv_context *) = nullptr;
    const spi::DeviceOps *runtime_ops = nullptr;
    bool open_attempted = false;
};

BootGroupSession::~BootGroupSession()
{
    close();
}

void BootGroupSession::close() noexcept
{
    if (closed_) return;
    closed_ = true;
    cleanup();
}

void BootGroupSession::cleanup() noexcept
{
    for (auto function = functions_.rbegin(); function != functions_.rend(); ++function) {
        auto &state = **function;
        const auto *ops = state.entry->bootstrap_ops;
        if (state.open_attempted && ops != nullptr && ops->close != nullptr) {
            call_callback([&] { return ops->close(state.context, {0, 0}); });
        }
        if (state.context != nullptr) {
            const auto destroy = state.destroy_state;
            if (destroy != nullptr) {
                try { destroy(state.context); } catch (...) {}
            }
            state.context = nullptr;
        }
    }
    functions_.clear();
    while (!owners_.empty()) owners_.pop_back();
}

dtv_context *BootGroupSession::context(dtv_function function) const
{
    for (const auto &state : functions_) {
        if (state->function == function) return state->context;
    }
    return nullptr;
}

const dtv_usb_io *BootGroupSession::io(dtv_function function) const
{
    for (const auto &state : functions_) {
        if (state->function == function) return state->io;
    }
    return nullptr;
}

const spi::DeviceOps *BootGroupSession::ops(dtv_function function) const
{
    for (const auto &state : functions_) {
        if (state->function == function) return state->runtime_ops;
    }
    return nullptr;
}

std::unique_ptr<BootGroupSession> open_boot_group(
    const UsbContext &usb, const BootGroupCandidate &group, dtv_device_id device_id,
    dtv_timeout timeout, const dtv_runtime_clock *requested_clock, const dtv_cancel *cancel,
    BootUsbProvider *provider,
    dtv_internal_result *result)
{
    if (result == nullptr) return nullptr;
    *result = make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    std::unique_ptr<BootGroupSession> session;
    try {
        if (group.model == nullptr || group.match == DTV_USB_GROUP_AMBIGUOUS ||
            (group.match != DTV_USB_GROUP_COMPLETE && group.match != DTV_USB_GROUP_INCOMPLETE) ||
            group.functions.empty() || group.snapshots.empty() ||
            group.model->functions == nullptr || group.model->function_count == 0) return nullptr;

        std::vector<std::uint32_t> physical_indices;
        std::vector<std::vector<UsbInterfaceClaim>> claims;
        for (const auto &selection : group.functions) {
            if (selection.candidate_index >= group.snapshots.size() ||
                !cold_identity_matches(*group.model, group.snapshots[selection.candidate_index])) {
                *result = make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
                return nullptr;
            }
            for (const auto &prior : group.functions) {
                if (&prior == &selection) break;
                if (prior.function == selection.function ||
                    (prior.candidate_index == selection.candidate_index &&
                     prior.interface_number == selection.interface_number)) return nullptr;
            }
            const auto &snapshot = group.snapshots[selection.candidate_index];
            const auto descriptors = std::count_if(snapshot.interfaces.begin(), snapshot.interfaces.end(),
                [&](const UsbInterfaceSnapshot &interface) {
                    return interface.info.interface_number == selection.interface_number &&
                           interface.info.alternate_setting == selection.alternate_setting;
                });
            if (descriptors != 1 || find_function_entry(*group.model, selection.function) == nullptr) {
                return nullptr;
            }
            auto found = std::find(physical_indices.begin(), physical_indices.end(),
                                   selection.candidate_index);
            if (found == physical_indices.end()) {
                physical_indices.push_back(selection.candidate_index);
                claims.emplace_back();
                found = physical_indices.end() - 1;
            }
            claims[static_cast<std::size_t>(found - physical_indices.begin())].push_back(
                {selection.interface_number, selection.alternate_setting});
        }

        const auto clock = requested_clock != nullptr ? *requested_clock : *system_runtime_clock();
        if (clock.monotonic_now_ns == nullptr) return nullptr;
        std::uint64_t now = 0;
        auto operation_result = call_callback([&] {
            now = clock.monotonic_now_ns(clock.user);
            return make_result(DTV_INTERNAL_RESULT_OK);
        });
        if (!succeeded(operation_result)) { *result = operation_result; return nullptr; }
        const auto deadline = deadline_after(now, timeout.timeout_ms);
        operation_result = remaining_timeout(clock, deadline, &timeout);
        if (!succeeded(operation_result)) { *result = operation_result; return nullptr; }

        session = std::unique_ptr<BootGroupSession>(new BootGroupSession);
        session->clock_ = clock;
        auto retain_failure = [&](dtv_internal_result failure) {
            *result = failure;
            session->open_result_ = failure;
            return std::move(session);
        };
        UsbBootProvider core_provider(usb);
        auto *active_provider = provider != nullptr ? provider : &core_provider;
        for (std::size_t i = 0; i < physical_indices.size(); ++i) {
            auto owner = std::make_unique<BootGroupSession::PhysicalOwner>();
            owner->candidate_index = physical_indices[i];
            operation_result = cancelled(clock, cancel);
            if (!succeeded(operation_result)) return retain_failure(operation_result);
            dtv_timeout remaining{};
            operation_result = remaining_timeout(clock, deadline, &remaining);
            if (!succeeded(operation_result)) return retain_failure(operation_result);
            owner->owner = active_provider->open_group(group.snapshots[physical_indices[i]], claims[i],
                                                       &session->clock_, result);
            if (!owner->owner) {
                if (succeeded(*result)) *result = make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
                return retain_failure(*result);
            }
            session->owners_.push_back(std::move(owner));
            if (!succeeded(*result) || session->owners_.back()->owner->io() == nullptr) {
                if (succeeded(*result)) *result = make_result(DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED);
                return retain_failure(*result);
            }
            operation_result = checkpoint(clock, deadline, cancel);
            if (!succeeded(operation_result)) return retain_failure(operation_result);
        }

        for (const auto &selection : group.functions) {
            const auto *entry = find_function_entry(*group.model, selection.function);
            auto state = std::make_unique<BootGroupSession::FunctionContext>();
            state->function = selection.function;
            state->candidate_index = selection.candidate_index;
            state->entry = entry;
            const auto owner_index = std::find(physical_indices.begin(), physical_indices.end(),
                                               selection.candidate_index) - physical_indices.begin();
            state->io = session->owners_[static_cast<std::size_t>(owner_index)]->owner->io();
            auto *create_state = entry->create_state != nullptr ? entry->create_state :
                                 group.model->create_state;
            const auto destroy_state = entry->destroy_state != nullptr ? entry->destroy_state :
                                       group.model->destroy_state;
            state->destroy_state = destroy_state;
            if ((create_state == nullptr) != (destroy_state == nullptr)) {
                return retain_failure(make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT));
            }
            if (create_state != nullptr) {
                operation_result = call_callback([&] {
                    return create_state(device_id, &state->context);
                });
                if (!succeeded(operation_result)) {
                    if (state->context != nullptr && destroy_state != nullptr) {
                        try { destroy_state(state->context); } catch (...) {}
                        state->context = nullptr;
                    }
                    return retain_failure(operation_result);
                }
            }
            session->functions_.push_back(std::move(state));
            auto &stored = *session->functions_.back();
            dtv_timeout remaining{};
            operation_result = remaining_timeout(clock, deadline, &remaining);
            if (!succeeded(operation_result)) return retain_failure(operation_result);
            std::vector<dtv_usb_interface_info> interfaces;
            const auto observation = candidate_view(group.snapshots[selection.candidate_index], &interfaces);
            dtv_open_args args{&group.model->model, selection.function, remaining, &observation,
                               selection.interface_number, selection.alternate_setting};
            const auto *bootstrap = entry->bootstrap_ops;
            if (bootstrap == nullptr || bootstrap->open == nullptr) {
                return retain_failure(make_result(DTV_INTERNAL_RESULT_UNSUPPORTED));
            }
            operation_result = cancelled(clock, cancel);
            if (!succeeded(operation_result)) return retain_failure(operation_result);
            stored.open_attempted = true;
            operation_result = call_callback([&] {
                return bootstrap->open(stored.context, stored.io, &args);
            });
            if (!succeeded(operation_result)) return retain_failure(operation_result);
            operation_result = checkpoint(clock, deadline, cancel);
            if (!succeeded(operation_result)) return retain_failure(operation_result);
        }
        *result = make_result(DTV_INTERNAL_RESULT_OK);
        session->open_result_ = *result;
        return session;
    } catch (...) {
        *result = exception_result();
        if (session) {
            session->open_result_ = *result;
            return session;
        }
        return nullptr;
    }
}

dtv_internal_result BootGroupSession::send_firmware(
    std::uint32_t candidate_index, dtv_bytes package, dtv_timeout timeout,
    const dtv_cancel *cancel)
{
    try {
        if (closed_) return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        if (!succeeded(open_result_)) return open_result_;
        if ((package.size != 0 && package.data == nullptr) || timeout.timeout_ms == 0) {
            return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
        }
        std::vector<FunctionContext *> targets;
        std::vector<FirmwarePackageImageSpec> specs;
        std::vector<std::size_t> image_bases;
        for (const auto &state : functions_) {
            if (state->candidate_index != candidate_index) continue;
            const auto *entry = state->entry;
            if (entry->image_count == 0 && entry->stage_count == 0) continue;
            if (entry->images == nullptr || entry->image_count == 0 ||
                (entry->stage_count != 0 && entry->stages == nullptr)) {
                return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
            }
            image_bases.push_back(specs.size());
            targets.push_back(state.get());
            for (std::size_t i = 0; i < entry->image_count; ++i) {
                if (entry->images[i].sha256 == nullptr) return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
                for (std::size_t prior = 0; prior < i; ++prior) {
                    const auto &left = entry->images[i];
                    const auto &right = entry->images[prior];
                    if (left.offset == right.offset && left.size == right.size &&
                        std::string_view(left.sha256) == right.sha256) {
                        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
                    }
                }
                specs.push_back({entry->images[i].offset, entry->images[i].size,
                                 entry->images[i].sha256});
            }
            for (std::size_t i = 0; i < entry->stage_count; ++i) {
                const auto &stage = entry->stages[i];
                if (stage.image_index >= entry->image_count ||
                    (stage.required_state != DTV_IDENTIFY_LOADER &&
                     stage.required_state != DTV_IDENTIFY_RUNTIME) ||
                    (stage.result_state != DTV_IDENTIFY_LOADER &&
                     stage.result_state != DTV_IDENTIFY_RUNTIME) ||
                    (i != 0 && stage.required_state != entry->stages[i - 1].result_state) ||
                    (i != 0 && stage.stage == entry->stages[i - 1].stage)) {
                    return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
                }
            }
            if (entry->stage_count != 0 &&
                entry->stages[entry->stage_count - 1].result_state != DTV_IDENTIFY_RUNTIME) {
                return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
            }
        }
        if (targets.empty() || specs.empty()) return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
        std::uint64_t now = 0;
        auto status = call_callback([&] {
            now = clock_.monotonic_now_ns(clock_.user);
            return make_result(DTV_INTERNAL_RESULT_OK);
        });
        if (!succeeded(status)) return status;
        const auto deadline = deadline_after(now, timeout.timeout_ms);
        status = checkpoint(clock_, deadline, cancel);
        if (!succeeded(status)) return status;
        VerifiedFirmwarePackage verified;
        const auto preflight = load_firmware_package(
            std::span<const std::uint8_t>(package.data, package.size), specs, &verified);
        const auto preflight_status = preflight_result(preflight);
        if (!succeeded(preflight_status)) return preflight_status;
        status = checkpoint(clock_, deadline, cancel);
        if (!succeeded(status)) return status;
        std::vector<dtv_identify_info> initial_states(targets.size());
        for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
            auto &state = *targets[target_index];
            const auto *bootstrap = state.entry->bootstrap_ops;
            if (bootstrap == nullptr || bootstrap->identify == nullptr) {
                return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            }
            status = cancelled(clock_, cancel);
            if (!succeeded(status)) return status;
            status = identify(*bootstrap, state.context, clock_, deadline,
                              &initial_states[target_index]);
            if (!succeeded(status)) return status;
            status = checkpoint(clock_, deadline, cancel);
            if (!succeeded(status)) return status;
            if (initial_states[target_index].state == DTV_IDENTIFY_UNKNOWN) {
                return make_result(DTV_INTERNAL_RESULT_NOT_READY);
            }
            if (initial_states[target_index].state == DTV_IDENTIFY_LOADER &&
                (state.entry->stage_count == 0 || bootstrap->firmware == nullptr)) {
                return make_result(DTV_INTERNAL_RESULT_NOT_READY);
            }
            if (initial_states[target_index].state == DTV_IDENTIFY_LOADER &&
                state.entry->stage_count != 0 &&
                state.entry->stages[0].required_state != initial_states[target_index].state) {
                return make_result(DTV_INTERNAL_RESULT_NOT_READY);
            }
        }
        for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
            auto &state = *targets[target_index];
            const auto *bootstrap = state.entry->bootstrap_ops;
            if (initial_states[target_index].state == DTV_IDENTIFY_LOADER) {
                for (std::size_t stage_index = 0; stage_index < state.entry->stage_count; ++stage_index) {
                    status = cancelled(clock_, cancel);
                    if (!succeeded(status)) return status;
                    dtv_timeout remaining{};
                    status = remaining_timeout(clock_, deadline, &remaining);
                    if (!succeeded(status)) return status;
                    const auto image = verified.image(image_bases[target_index] +
                                                      state.entry->stages[stage_index].image_index);
                    dtv_firmware_result firmware_result{};
                    firmware_result.transition = UINT32_MAX;
                    status = call_callback([&] {
                        return bootstrap->firmware(state.context,
                            state.entry->stages[stage_index].stage,
                            {image.data(), image.size()}, &firmware_result, remaining);
                    });
                    if (!succeeded(status)) return status;
                    status = checkpoint(clock_, deadline, cancel);
                    if (!succeeded(status)) return status;
                    if (firmware_result.transition != DTV_FIRMWARE_SAME_HANDLE) {
                        return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
                    }
                    dtv_identify_info stage_state{};
                    status = identify(*bootstrap, state.context, clock_, deadline, &stage_state);
                    if (!succeeded(status)) return status;
                    status = checkpoint(clock_, deadline, cancel);
                    if (!succeeded(status)) return status;
                    if (stage_state.state != state.entry->stages[stage_index].result_state) {
                        return make_result(DTV_INTERNAL_RESULT_NOT_READY);
                    }
                }
                status = cancelled(clock_, cancel);
                if (!succeeded(status)) return status;
                dtv_identify_info info{};
                status = identify(*bootstrap, state.context, clock_, deadline, &info);
                if (!succeeded(status)) return status;
                status = checkpoint(clock_, deadline, cancel);
                if (!succeeded(status)) return status;
                if (info.state != DTV_IDENTIFY_RUNTIME) return make_result(DTV_INTERNAL_RESULT_NOT_READY);
                const auto *runtime = find_function_firmware_entry(*state.entry, info.firmware_key);
                if (runtime == nullptr) return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
                state.runtime_ops = runtime->ops;
                continue;
            }
            const auto &info = initial_states[target_index];
            if (info.state != DTV_IDENTIFY_RUNTIME) return make_result(DTV_INTERNAL_RESULT_NOT_READY);
            const auto *runtime = find_function_firmware_entry(*state.entry, info.firmware_key);
            if (runtime == nullptr) return make_result(DTV_INTERNAL_RESULT_UNSUPPORTED);
            state.runtime_ops = runtime->ops;
        }
        return make_result(DTV_INTERNAL_RESULT_OK);
    } catch (...) {
        return exception_result();
    }
}

}
