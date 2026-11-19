/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_USB_HPP
#define DTV_INTERNAL_USB_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <dtv/dtv.h>
#include <dtv/spi/types.h>

namespace dtv::internal {

class UsbOperationScope {
public:
    UsbOperationScope(const dtv_runtime_clock *, const dtv_cancel *, std::uint64_t deadline_ns);
    ~UsbOperationScope();
    UsbOperationScope(const UsbOperationScope &) = delete;
    UsbOperationScope &operator=(const UsbOperationScope &) = delete;

private:
    const dtv_runtime_clock *previous_clock_ = nullptr;
    const dtv_cancel *previous_cancel_ = nullptr;
    std::uint64_t previous_deadline_ns_ = 0;
};

enum class UsbCloseStatus { open, closing, pending, closed };
enum class UsbOperationTerminalReason {
    none, completed, cancelled, endpoint_cancelled, deadline, transfer_timeout, closing, disconnected
};
UsbOperationTerminalReason current_usb_terminal_reason();

struct UsbTestDeviceFactory;
class UsbTransferBackend;

struct UsbInterfaceSnapshot {
    dtv_usb_interface_info info{};
    std::vector<dtv_usb_endpoint_info> endpoints;
};

struct UsbInterfaceClaim {
    std::uint8_t interface_number;
    std::uint8_t alternate_setting;
};

struct UsbConnectionIdentity {
    std::uint64_t filesystem = 0;
    std::uint64_t inode = 0;
    std::uint64_t device = 0;
    bool valid = false;

    bool operator==(const UsbConnectionIdentity &) const = default;
};

struct UsbDeviceSnapshot {
    struct State;
    struct State;
    std::uint16_t vendor_id;
    std::uint16_t product_id;
    std::uint16_t device_version;
    std::uint8_t device_class;
    std::uint8_t device_subclass;
    std::uint8_t device_protocol;
    std::uint8_t bus_number;
    std::uint8_t device_address;
    std::uint8_t configuration_count;
    std::uint64_t candidate_id = 0;
    std::uint64_t connection_token = 0;
    dtv_usb_token selection_token = 0;
    std::vector<std::uint8_t> port_path;
    std::vector<UsbInterfaceSnapshot> interfaces;

private:
    std::shared_ptr<State> state_;
    friend class UsbContext;
    friend struct UsbTestDeviceFactory;
};

struct UsbClaimDeviceInput {
    std::uint64_t connection_epoch = 0;
    UsbDeviceSnapshot snapshot;
    std::vector<UsbInterfaceClaim> interfaces;
};

class UsbClaimLeaseGroup {
public:
    struct State;
    UsbClaimLeaseGroup();
    ~UsbClaimLeaseGroup();
    UsbClaimLeaseGroup(const UsbClaimLeaseGroup &) = delete;
    UsbClaimLeaseGroup &operator=(const UsbClaimLeaseGroup &) = delete;
    UsbClaimLeaseGroup(UsbClaimLeaseGroup &&) noexcept;
    UsbClaimLeaseGroup &operator=(UsbClaimLeaseGroup &&) noexcept;

    dtv_internal_result release() noexcept;
    std::uint64_t deadline_ns() const;
    std::size_t size() const;

private:
    std::unique_ptr<State> state_;
    friend class UsbContext;
    friend class EnclosureRuntime;
};

enum class UsbRuntimeTerminal {
    completed, cancelled, deadline, endpoint_cancelled, stopped, failed
};

struct UsbRuntimeCompletion {
    std::uint64_t request_id = 0;
    std::uint64_t submit_sequence = 0;
    std::uint64_t completion_sequence = 0;
    std::uint64_t generation = 0;
    std::size_t handle_index = 0;
    std::uint8_t endpoint = 0;
    UsbRuntimeTerminal terminal = UsbRuntimeTerminal::failed;
    dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    std::size_t actual_length = 0;
    bool reached_backend = false;
    std::vector<unsigned char> bytes;
};

struct UsbRuntimePacket {
    std::uint64_t request_id = 0;
    std::uint64_t submit_sequence = 0;
    std::uint64_t generation = 0;
    std::size_t handle_index = 0;
    std::uint8_t endpoint = 0;
    std::size_t actual_length = 0;
    std::span<const unsigned char> bytes;
    UsbRuntimeTerminal terminal = UsbRuntimeTerminal::completed;
    dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
};

struct UsbRuntimeStopResult {
    bool pending = false;
    dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
};

class EnclosureRuntime {
public:
    struct State;
    static std::unique_ptr<EnclosureRuntime> start(
        UsbClaimLeaseGroup &&, dtv_internal_result *result);
    ~EnclosureRuntime();
    EnclosureRuntime(const EnclosureRuntime &) = delete;
    EnclosureRuntime &operator=(const EnclosureRuntime &) = delete;

    dtv_internal_result submit_bulk_out(
        std::size_t handle_index, std::uint8_t endpoint,
        std::span<const unsigned char> bytes, std::uint64_t generation,
        const dtv_runtime_clock *, const dtv_cancel *, std::uint64_t deadline_ns,
        std::uint64_t *request_id);
    dtv_internal_result submit_bulk_in(
        std::size_t handle_index, std::uint8_t endpoint, std::size_t capacity,
        std::uint64_t generation, const dtv_runtime_clock *, const dtv_cancel *,
        std::uint64_t deadline_ns, std::uint64_t *request_id);
    dtv_internal_result submit_shared_bulk_in(
        std::size_t handle_index, std::uint8_t endpoint, std::size_t capacity,
        std::uint64_t generation,
        dtv_internal_result (*)(void *, const UsbRuntimePacket *), void *,
        std::uint64_t *request_id);
    dtv_internal_result cancel(std::uint64_t request_id);
    dtv_internal_result wait_request(std::uint64_t request_id,
                                     const dtv_runtime_clock *, std::uint64_t deadline_ns,
                                     UsbRuntimeTerminal *terminal);
    dtv_internal_result next_completion(UsbRuntimeCompletion *, bool *available);
    dtv_internal_result take_completion(std::uint64_t request_id,
                                        UsbRuntimeCompletion *, bool *available);
    UsbRuntimeStopResult stop(const dtv_runtime_clock *, std::uint64_t deadline_ns);
    dtv_internal_result join_release();
    std::size_t worker_count() const;

private:
    explicit EnclosureRuntime(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
};

void set_enclosure_runtime_start_failure_for_test(bool);

class UsbDevice;

class UsbContext {
public:
    struct State;
    UsbContext();
    static UsbContext scan_only(std::shared_ptr<UsbTransferBackend> = {});
    ~UsbContext();
    UsbContext(const UsbContext &) = delete;
    UsbContext &operator=(const UsbContext &) = delete;
    UsbContext(UsbContext &&) noexcept;
    UsbContext &operator=(UsbContext &&) noexcept;

    dtv_internal_result result() const;
    std::vector<UsbDeviceSnapshot> enumerate(dtv_internal_result *result = nullptr,
                                            dtv_token_of_callback = nullptr,
                                            void *token_user = nullptr) const;
    std::vector<UsbDeviceSnapshot> enumerate_fds(std::span<const std::int32_t>,
                                                 dtv_internal_result *result = nullptr) const;
    dtv_internal_result acquire_claims(std::span<const UsbClaimDeviceInput>,
                                       const dtv_runtime_clock *, const dtv_cancel *,
                                       std::uint64_t deadline_ns,
                                       UsbClaimLeaseGroup *out) const;
    std::unique_ptr<UsbDevice> open(const UsbDeviceSnapshot &, int interface_number,
                                    const dtv_runtime_clock *clock = nullptr) const;
    std::unique_ptr<UsbDevice> open(const UsbDeviceSnapshot &,
                                    std::span<const UsbInterfaceClaim>,
                                    const dtv_runtime_clock *clock = nullptr) const;

private:
    explicit UsbContext(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
    friend struct UsbTestDeviceFactory;
};

class UsbDevice {
public:
    struct State;
    ~UsbDevice();
    UsbDevice(const UsbDevice &) = delete;
    UsbDevice &operator=(const UsbDevice &) = delete;

    dtv_internal_result result() const;
    const dtv_usb_io *io() const;
    dtv_internal_result run_close_callback(
        std::function<dtv_internal_result()> callback, dtv_timeout timeout);
    void begin_close();
    void finish_close(dtv_internal_result);
    UsbCloseStatus close_status() const;
    dtv_internal_result close_result() const;

private:
    explicit UsbDevice(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
    friend class UsbContext;
    friend struct UsbTestDeviceFactory;
    friend std::unique_ptr<UsbDevice> open_usb_fd(int, int, dtv_internal_result *);
};

/* The input descriptor is borrowed; the Core duplicates and owns its wrapper FD. */
std::unique_ptr<UsbDevice> open_usb_fd(int descriptor, int interface_number,
                                       dtv_internal_result *result);

}

#endif
