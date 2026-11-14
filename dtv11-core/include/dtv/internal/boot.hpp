/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_BOOT_HPP
#define DTV_INTERNAL_BOOT_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include <dtv/internal/firmware.hpp>
#include <dtv/internal/discovery.hpp>
#include <dtv/internal/usb.hpp>
#include <dtv/spi/device_ops.hpp>

namespace dtv::internal {

struct BootStage {
    uint32_t stage;
    std::size_t image_index;
};

class BootUsbOwner {
public:
    virtual ~BootUsbOwner() = default;
    virtual const dtv_usb_io *io() const = 0;
    virtual void begin_close() {}
    virtual dtv_internal_result run_close_callback(
        std::function<dtv_internal_result()> callback, dtv_timeout timeout)
    {
        (void)timeout;
        return callback ? callback() : dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
    virtual void finish_close(dtv_internal_result) {}
    virtual UsbCloseStatus close_status() const { return UsbCloseStatus::open; }
    virtual dtv_internal_result close_result() const
    {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    }
};

class BootUsbProvider {
public:
    virtual ~BootUsbProvider() = default;
    virtual std::unique_ptr<BootUsbOwner> open(
        const UsbDeviceSnapshot &, int, const dtv_runtime_clock *,
        dtv_internal_result *) = 0;
    virtual std::unique_ptr<BootUsbOwner> open_group(
        const UsbDeviceSnapshot &, std::span<const UsbInterfaceClaim>,
        const dtv_runtime_clock *, dtv_internal_result *);
};

struct BootRequest {
    const UsbDeviceSnapshot *selected_candidate;
    int interface_number;
    const spi::ModelEntry *model;
    dtv_device_id device_id;
    dtv_function function;
    std::span<const FirmwareImageSpec> images;
    std::span<const BootStage> stages;
    dtv_timeout timeout;
    const dtv_cancel *cancel;
    const dtv_runtime_clock *clock;
    BootUsbProvider *provider;
};

class BootSession {
public:
    ~BootSession();
    BootSession(const BootSession &) = delete;
    BootSession &operator=(const BootSession &) = delete;

    const spi::DeviceOps *ops() const { return ops_; }
    const dtv_usb_io *io() const { return usb_ ? usb_->io() : nullptr; }
    dtv_context *context() const { return instance_ ? instance_->context() : nullptr; }

private:
    friend std::unique_ptr<BootSession> boot_selected_candidate(
        const UsbContext &, const BootRequest &, dtv_internal_result *);
    BootSession() = default;
    void cleanup() noexcept;

    const spi::DeviceOps *ops_ = nullptr;
    const spi::DeviceOps *opened_ops_ = nullptr;
    std::unique_ptr<spi::ProductInstance> instance_;
    std::unique_ptr<BootUsbOwner> usb_;
    dtv_runtime_clock clock_{};
    bool open_attempted_ = false;
};

class BootGroupSession {
public:
    ~BootGroupSession();
    BootGroupSession(const BootGroupSession &) = delete;
    BootGroupSession &operator=(const BootGroupSession &) = delete;

    dtv_internal_result send_firmware(std::uint32_t candidate_index, dtv_bytes package,
                                      dtv_timeout timeout, const dtv_cancel *cancel = nullptr);
    dtv_internal_result open_result() const { return open_result_; }
    void close() noexcept;
    dtv_internal_result close(dtv_timeout timeout) noexcept;
    UsbCloseStatus close_status() const;
    dtv_internal_result close_result() const;
    dtv_internal_result release() noexcept;
    dtv_context *context(dtv_function function) const;
    const dtv_usb_io *io(dtv_function function) const;
    const spi::DeviceOps *ops(dtv_function function) const;

private:
    friend std::unique_ptr<BootGroupSession> open_boot_group(
        const UsbContext &, const BootGroupCandidate &, dtv_device_id, dtv_timeout,
        const dtv_runtime_clock *, const dtv_cancel *, BootUsbProvider *, dtv_internal_result *);
    struct PhysicalOwner;
    struct FunctionContext;
    BootGroupSession() = default;
    void cleanup() noexcept;
    std::vector<std::unique_ptr<PhysicalOwner>> owners_;
    std::vector<std::unique_ptr<FunctionContext>> functions_;
    dtv_runtime_clock clock_{};
    dtv_internal_result open_result_ = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    std::atomic<bool> close_started_{false};
    std::atomic<bool> closed_{false};
    std::atomic<UsbCloseStatus> close_status_{UsbCloseStatus::open};
    mutable std::mutex close_mutex_;
    std::size_t close_waiters_ = 0;
    std::uint64_t close_deadline_ns_ = 0;
    dtv_internal_result close_result_ = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
};

const spi::FirmwareEntry *find_firmware_entry(const spi::ModelEntry &model,
                                              dtv_fw_key key) noexcept;
std::unique_ptr<BootSession> boot_selected_candidate(
    const UsbContext &usb, const BootRequest &request, dtv_internal_result *result);
std::unique_ptr<BootGroupSession> open_boot_group(
    const UsbContext &, const BootGroupCandidate &, dtv_device_id, dtv_timeout,
    const dtv_runtime_clock *, const dtv_cancel *, BootUsbProvider *, dtv_internal_result *);

}

#endif
