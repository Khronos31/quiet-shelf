/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_L3_REGISTRY_HPP
#define DTV_INTERNAL_L3_REGISTRY_HPP

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <dtv/dtv.h>
#include <dtv/internal/discovery.hpp>
#include <dtv/internal/usb.hpp>
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/types.h>

namespace dtv::internal {

struct EnclosureOpenDescription {
    const dtv::spi::ModelEntry *model;
    std::vector<UsbClaimDeviceInput> inputs;
    std::vector<dtv_function> functions;
};

class EnclosureRegistry {
public:
    struct ReceiverSignalSnapshot {
        dtv_enclosure *enclosure = nullptr;
        std::uint64_t connection_epoch = 0;
        std::uint32_t receiver_index = 0;
        std::uint16_t source_stride_bytes = 0;
        std::uint8_t source_sync_offset_bytes = 0;
        std::uint32_t receiver_systems = 0;
        std::uint32_t known_operations = 0;
        std::uint32_t supported_operations = 0;
        std::uint64_t operation_epoch = 0;
        std::uint32_t enclosure_status = DTV_ENCLOSURE_DISCONNECTED;
        std::uint32_t signal_support = DTV_UNKNOWN;
        bool lock_known = false;
        std::uint32_t lock_reason = DTV_MEASUREMENT_REASON_NOT_SAMPLED;
        std::uint32_t lock_state = 0;
    };

    using BeforeJournalAllocation = std::function<void()>;

    explicit EnclosureRegistry(BeforeJournalAllocation = {});
    ~EnclosureRegistry();
    EnclosureRegistry(const EnclosureRegistry &) = delete;
    EnclosureRegistry &operator=(const EnclosureRegistry &) = delete;

    dtv_internal_result refresh(CompleteGroupBatch, const CompleteGroupBatch *token_selection = nullptr);
    dtv_internal_result refresh_by_fds(CompleteGroupBatch);
    const std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &live_slots() const;
    const dtv_ungrouped_device *const *ungrouped_view() const;
    std::size_t capacity() const;
    bool scan_complete() const;
    void begin_public_boundary();
    void set_ungrouped_allocation_hook_for_test(std::function<void()>);
    void set_token_allocation_hook_for_test(std::function<void()>);
    void set_receiver_allocation_hook_for_test(std::function<void()>);
    std::size_t slot_of(const dtv_enclosure *) const;
    void project_product_slots(
        const char *, std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &) const;
    const std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &token_slots() const;
    const std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &fd_slots() const;
    void project_token_product_slots(
        const char *, std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &) const;
    void project_fd_product_slots(
        const char *, std::array<dtv_enclosure *, DTV_MAX_ENCLOSURES> &) const;
    std::uint32_t status(const dtv_enclosure *) const;
    std::uint32_t card_state(const dtv_enclosure *) const;
    std::uint32_t card_generation(const dtv_enclosure *) const;
    dtv_device *card_reader_device(const dtv_enclosure *) const;
    dtv_internal_result begin_card_reset(dtv_enclosure *, std::shared_ptr<void> *,
                                          std::uint64_t *, std::uint64_t *);
    dtv_internal_result finish_card_reset(dtv_enclosure *, std::uint64_t, std::uint64_t,
                                          std::span<const std::uint8_t>);
    dtv_internal_result card_atr_view(const dtv_enclosure *, const std::uint8_t **,
                                      std::uint64_t *) const;
    std::uint32_t enclosure_id(const dtv_enclosure *) const;
    const char *product(const dtv_enclosure *) const;
    const char *model(const dtv_enclosure *) const;
    dtv_device *const *device_list(const dtv_enclosure *) const;
    const dtv_candidate *const *candidates(const dtv_enclosure *) const;
    std::span<dtv_device *const> devices(const dtv_enclosure *) const;
    std::uint32_t device_id(const dtv_device *) const;
    std::uint16_t vendor_id(const dtv_device *) const;
    std::uint16_t product_id(const dtv_device *) const;
    dtv_receiver *const *receiver_list(const dtv_device *) const;
    dtv_internal_result update_receiver_declarations(
        dtv_enclosure *, std::span<const dtv::spi::model_information::device_receiver_information>);
    std::uint32_t receiver_index(const dtv_receiver *) const;
    std::uint32_t receiver_local(const dtv_receiver *) const;
    std::uint32_t receiver_systems(const dtv_receiver *) const;
    std::uint32_t receiver_ts_scrambling(const dtv_receiver *) const;
    std::int32_t receiver_support(const dtv_receiver *, std::uint32_t,
                                  std::uint32_t *, dtv_error *) const;
    dtv_internal_result receiver_signal_snapshot(const dtv_receiver *, ReceiverSignalSnapshot *) const;
    dtv_internal_result begin_receiver_tune(const dtv_receiver *, std::uint64_t connection_epoch,
        std::uint64_t previous_operation_epoch, std::uint64_t *operation_epoch);
    const std::uint32_t *bus(const dtv_device *) const;
    const std::uint32_t *address(const dtv_device *) const;
    const char *port(const dtv_device *) const;
    bool hold(dtv_enclosure *);
    bool drop(dtv_enclosure *);
    bool retain_open(dtv_enclosure *);
    bool release_open(dtv_enclosure *);
    dtv_internal_result bind_card_reader(dtv_enclosure *, std::uint64_t connection_epoch);
    dtv_internal_result observe_card_state(dtv_enclosure *, std::uint64_t connection_epoch,
                                           std::uint32_t card_state);
    dtv_internal_result observe_receiver_lock(dtv_enclosure *, std::uint64_t connection_epoch,
                                              std::uint64_t operation_epoch,
                                              std::uint32_t receiver_index,
                                              std::uint32_t lock_state);
    dtv_internal_result open_description(dtv_enclosure *, EnclosureOpenDescription *) const;
    dtv_internal_result claim_inputs(dtv_enclosure *, std::vector<UsbClaimDeviceInput> *) const;
    dtv_internal_result enqueue_event(std::uint32_t kind, dtv_enclosure *, dtv_receiver *,
                                     std::shared_ptr<void> receiver_owner = {});
    dtv_internal_result next_event(dtv_event *, bool *available);
    bool resource_limited() const;

private:
    dtv_internal_result scan_failed();
    struct State;
    std::unique_ptr<State> state_;
    friend class DiscoverySession;
};

class DiscoverySession {
public:
    explicit DiscoverySession(EnclosureRegistry &);
    dtv_internal_result refresh();
    dtv_internal_result refresh(CompleteGroupBatch);
    dtv_internal_result refresh(CompleteGroupBatch, CompleteGroupBatch);
    dtv_internal_result scan_with_token_callback(
        dtv_token_of_callback, void *, CompleteGroupBatch *);
    dtv_internal_result scan_with_fds(std::span<const std::int32_t>, CompleteGroupBatch *);
    void set_context_for_test(UsbContext);
    dtv_internal_result fail_scan_for_test(dtv_internal_result);
    std::size_t worker_count_for_test() const;
    dtv_internal_result acquire_claims(std::span<const UsbClaimDeviceInput>,
                                       const dtv_runtime_clock *, const dtv_cancel *,
                                       std::uint64_t, UsbClaimLeaseGroup *) const;

private:
    UsbContext context_;
    EnclosureRegistry *registry_;
};

void set_public_discovery_batch_for_test(CompleteGroupBatch);
void set_public_discovery_context_for_test(UsbContext, CompleteGroupBatch);
void set_public_discovery_journal_hook_for_test(std::function<void()>);
void set_public_discovery_scan_error_for_test(dtv_internal_result);
void set_public_discovery_ungrouped_allocation_hook_for_test(std::function<void()>);
void set_public_discovery_usb_backend_for_test(std::shared_ptr<UsbTransferBackend>);
void set_public_discovery_token_allocation_hook_for_test(std::function<void()>);
void set_public_discovery_receiver_allocation_hook_for_test(std::function<void()>);
dtv_internal_result update_public_discovery_receiver_declarations_for_test(
    dtv_enclosure *,
    std::span<const dtv::spi::model_information::device_receiver_information>);
void set_public_operation_allocation_failure_for_test(bool);
dtv_internal_result run_public_operation_for_test(
    dtv_enclosure *, const dtv::spi::PublicOperationRequest &, dtv_timeout,
    std::span<unsigned char>, std::size_t *);
std::int32_t run_public_stream_operation(
    dtv_stream *, std::uint32_t, std::span<const unsigned char>, dtv_timeout,
    std::span<unsigned char>, std::size_t *, dtv_error *);
std::int32_t run_public_card_operation(
    dtv_enclosure *, std::uint32_t, std::span<const unsigned char>, dtv_timeout,
    std::span<unsigned char>, std::size_t *, dtv_error *);

}

#endif
