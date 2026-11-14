/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_SPI_DEVICE_OPS_HPP
#define DTV_SPI_DEVICE_OPS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include <dtv/cli.h>
#include <dtv/dtv.h>
#include <dtv/internal/result.hpp>
#include <dtv/spi/types.h>

namespace dtv::spi {

struct DeviceOps {
    dtv_internal_result (*open)(dtv_context *, const dtv_usb_io *, const dtv_open_args *);
    dtv_internal_result (*identify)(dtv_context *, dtv_identify_info *, dtv_timeout);
    dtv_internal_result (*firmware)(dtv_context *, uint32_t, dtv_bytes, dtv_firmware_result *,
                           dtv_timeout);
    dtv_internal_result (*tune_t)(dtv_context *, dtv_spi_receiver_id, uint32_t, uint64_t,
                         dtv_spi_tune_result *, dtv_timeout);
    dtv_internal_result (*signal)(dtv_context *, dtv_spi_receiver_id, dtv_signal_result *, dtv_timeout);
    dtv_internal_result (*ts_prepare)(dtv_context *, dtv_spi_receiver_id, uint64_t,
                             const dtv_stream_program *, dtv_timeout);
    dtv_internal_result (*service_tick)(dtv_context *, const dtv_usb_io *, uint64_t, dtv_timeout);
    dtv_internal_result (*ts_transform)(dtv_context *, dtv_spi_receiver_id, dtv_bytes, dtv_buffer *);
    dtv_internal_result (*ts_stop_device)(dtv_context *, dtv_spi_receiver_id, dtv_timeout);
    dtv_internal_result (*close)(dtv_context *, dtv_timeout);
    dtv_internal_result (*card_uart_write_byte)(dtv_context *, uint8_t, dtv_timeout);
    dtv_internal_result (*card_uart_read_byte)(dtv_context *, uint8_t *, dtv_timeout);
    dtv_internal_result (*card_frame_write)(dtv_context *, dtv_bytes, dtv_timeout);
    dtv_internal_result (*card_frame_read)(dtv_context *, dtv_buffer *, dtv_timeout);
};

struct FirmwareEntry {
    dtv_fw_key key;
    const DeviceOps *ops;
};

struct FirmwareManifestImage {
    std::uint64_t offset;
    std::uint64_t size;
    const char *sha256;
};

struct FunctionFirmwareStage {
    std::uint32_t stage;
    std::size_t image_index;
    std::uint32_t required_state = DTV_IDENTIFY_LOADER;
    std::uint32_t result_state = DTV_IDENTIFY_LOADER;
};

struct FunctionEntry {
    dtv_function function;
    const DeviceOps *bootstrap_ops;
    const FirmwareEntry *firmware;
    std::size_t firmware_count;
    const FirmwareManifestImage *images;
    std::size_t image_count;
    const FunctionFirmwareStage *stages;
    std::size_t stage_count;
    dtv_internal_result (*create_state)(dtv_device_id, dtv_context **);
    void (*destroy_state)(dtv_context *);
    /* 0ならpackage全長の指定なし。指定時は入力全体と一致させる。 */
    std::uint64_t expected_package_size = 0;
    /* nullptrならpackage全体hashの指定なし。imageごとのhash検証とは独立する。 */
    const char *expected_package_sha256 = nullptr;
};

struct OperationRequirement {
    dtv_spi_receiver_id receiver;
    /* dtv_spi_operationを使用する。公開DTV_OP_*と値・意味を共有する。 */
    std::uint32_t operation;
    const dtv_function *functions;
    std::size_t function_count;
};

struct PublicOpenPlan {
    std::size_t handle_index;
    std::uint8_t endpoint;
    std::size_t buffer_capacity;
    std::size_t shared_in_depth = 0;
    std::size_t prestream_sink_limit = 0;
    std::size_t reply_capacity = 0;
    bool card_reader_source = false;
    bool receiver_lock_source = false;
};

struct PublicOpenPacket {
    std::size_t handle_index;
    std::uint8_t endpoint;
    std::uint64_t submit_sequence;
    std::uint64_t generation;
    std::size_t actual_length;
    std::span<const unsigned char> bytes;
};

enum class PublicOpenMessageKind { ignore, notification, control_reply, source_chunk, source_end };
enum class PublicOpenNotificationKind { none, card_state, receiver_lock };

struct PublicOpenMessage {
    PublicOpenMessageKind kind = PublicOpenMessageKind::ignore;
    PublicOpenNotificationKind notification_kind = PublicOpenNotificationKind::none;
    std::uint64_t resource_scope = 0;
    std::uint64_t transaction_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t stream_generation = 0;
    std::uint64_t operation_epoch = 0;
    std::uint32_t card_state = 0;
    std::uint32_t receiver_index = 0;
    std::uint32_t lock_state = 0;
    std::span<const unsigned char> payload;
};

struct PublicStreamRequest {
    std::uint32_t receiver_index = 0;
    std::uint64_t connection_epoch = 0;
    std::uint64_t stream_generation = 0;
    std::uint16_t source_stride_bytes = 0;
    std::uint8_t source_sync_offset_bytes = 0;
    std::size_t queue_capacity = 0;
    const std::atomic<bool> *cancelled = nullptr;
};

struct PublicStreamPlan {
    bool supported = false;
    bool ready = false;
    bool arguments_valid = false;
    std::span<const std::uint64_t> resource_keys;
};

enum class PublicOpenWaitResult { completed, cancelled, deadline, stopped, failed };

struct PublicOpenSession {
    void *user = nullptr;
    dtv_internal_result (*register_reply)(void *, std::uint64_t, std::uint64_t,
                                          std::uint64_t, std::uint64_t *) = nullptr;
    dtv_internal_result (*wait_reply)(void *, std::uint64_t, const std::atomic<bool> *,
                                      dtv_timeout, PublicOpenWaitResult *,
                                      std::span<unsigned char>, std::size_t *) = nullptr;
    dtv_internal_result (*cancel_reply)(void *, std::uint64_t) = nullptr;
    dtv_internal_result (*generation_for_handle)(void *, std::size_t,
                                                  std::uint64_t *) = nullptr;
    dtv_internal_result (*abandon_reply)(void *, std::uint64_t) = nullptr;
};

enum class PublicOperationTargetKind : std::uint32_t { enclosure, receiver, card, stream };

struct PublicOperationRequest {
    std::uint32_t operation;
    PublicOperationTargetKind target_kind;
    std::uint64_t target_id;
    std::span<const unsigned char> arguments;
};

struct PublicOperationPlan {
    bool supported = false;
    bool ready = false;
    bool arguments_valid = false;
    std::size_t handle_index = 0;
    std::uint8_t bulk_out_endpoint = 0;
    std::uint64_t resource_scope = 0;
    std::uint64_t transaction_id = 0;
    std::size_t reply_capacity = 0;
    std::span<const std::uint64_t> resource_keys;
    std::span<const unsigned char> output;
};

struct CardByteWrite {
    std::span<const unsigned char> bytes;
};

struct CardByteRead {
    std::size_t capacity = 0;
};

struct CardT1BlockExchange {
    std::span<const unsigned char> block;
    std::size_t response_capacity = 0;
};

using CardTransportOperation = std::variant<CardByteWrite, CardByteRead, CardT1BlockExchange>;

struct CardTransportContext {
    dtv_device *reader = nullptr;
    std::uint64_t connection_epoch = 0;
    std::uint64_t request_epoch = 0;
};

struct CardTransportRequest {
    CardTransportContext context;
    CardTransportOperation operation;
};

struct CardIoOutcome {
    std::size_t tx_bytes_done = 0;
    std::size_t rx_bytes_done = 0;
    bool reply_complete = false;
    bool result_known = true;
    dtv_internal_result transport_error = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
};

struct PublicCardTransportPlan {
    PublicOperationPlan operation;
};

struct PublicTuneRequest {
    std::uint32_t receiver_index = 0;
    std::uint64_t connection_epoch = 0;
    std::uint64_t previous_operation_epoch = 0;
    std::uint32_t system = 0;
    std::uint64_t frequency_hz = 0;
    bool has_tune_options = false;
    dtv_tune_options tune_options{};
    dtv_timeout timeout{};
    const std::atomic<bool> *cancelled = nullptr;
};

struct PublicTunePlan {
    bool supported = false;
    bool ready = false;
    bool arguments_valid = false;
    std::span<const std::uint64_t> resource_keys;
};

struct PublicTuneSession {
    void *user = nullptr;
    /* Call immediately before the first device-visible effect; this advances the receiver tune epoch. */
    dtv_internal_result (*begin_effect)(void *, std::uint64_t *) = nullptr;
};

struct PublicOpenOps {
    dtv_internal_result (*plan)(std::span<const dtv_usb_candidate_info>,
                                std::span<const dtv_function>,
                                std::span<PublicOpenPlan>, std::size_t *);
    dtv_internal_result (*identify)(dtv_context *, std::span<const unsigned char>,
                                    dtv_timeout, const std::atomic<bool> *);
    dtv_internal_result (*stop)(dtv_context *, dtv_timeout);
    dtv_internal_result (*demux)(dtv_context *, const PublicOpenPacket *, PublicOpenMessage *);
    dtv_internal_result (*identify_shared)(dtv_context *, std::span<const unsigned char>,
                                           dtv_timeout, const std::atomic<bool> *,
                                           PublicOpenSession *);
    dtv_internal_result (*plan_operation)(dtv_context *, const PublicOperationRequest *,
                                          PublicOperationPlan *);
    dtv_internal_result (*plan_tune)(dtv_context *, const PublicTuneRequest *, PublicTunePlan *) = nullptr;
    dtv_internal_result (*tune_by_frequency)(dtv_context *, const PublicTuneRequest *,
                                             PublicTuneSession *) = nullptr;
    dtv_internal_result (*prepare_stream)(dtv_context *, const PublicStreamRequest *) = nullptr;
    dtv_internal_result (*start_stream)(dtv_context *, const PublicStreamRequest *, bool *) = nullptr;
    dtv_internal_result (*stop_stream)(dtv_context *, const PublicStreamRequest *, dtv_timeout) = nullptr;
    dtv_internal_result (*after_stream_drain)(dtv_context *, const PublicStreamRequest *) = nullptr;
    dtv_internal_result (*plan_stream)(dtv_context *, const PublicStreamRequest *,
                                       PublicStreamPlan *) = nullptr;
    dtv_internal_result (*transform_stream)(dtv_context *, const PublicStreamRequest *,
                                            std::span<const unsigned char>,
                                            std::span<unsigned char>, std::size_t *,
                                            std::size_t *) = nullptr;
    dtv_internal_result (*plan_card_transport)(dtv_context *, const CardTransportRequest *,
                                               PublicCardTransportPlan *) = nullptr;
};

struct ModelEntry {
    dtv_model_info model;
    const FirmwareEntry *firmware;
    // Zero entries leave the model unbound; there is no default operations table.
    std::size_t firmware_count;
    dtv_internal_result (*create_state)(dtv_device_id, dtv_context **);
    void (*destroy_state)(dtv_context *);
    const DeviceOps *bootstrap_ops;
    /* Explicit loader/cold USB identities; runtime model IDs are never inferred as cold IDs. */
    const dtv_usb_identity *cold_identities = nullptr;
    std::size_t cold_identity_count = 0;
    dtv_select_usb_group_fn select_usb_group = nullptr;
    const FunctionEntry *functions = nullptr;
    std::size_t function_count = 0;
    const OperationRequirement *operation_requirements = nullptr;
    std::size_t operation_requirement_count = 0;
    /* 0/0 means not reported; TS sources use 188/0 or 192/4. */
    std::uint16_t source_stride_bytes = 0;
    std::uint8_t source_sync_offset_bytes = 0;
    const PublicOpenOps *public_open = nullptr;
};

constexpr bool valid_source_metadata(const ModelEntry &model)
{
    return (model.source_stride_bytes == 0 && model.source_sync_offset_bytes == 0) ||
           (model.source_stride_bytes == 188 && model.source_sync_offset_bytes == 0) ||
           (model.source_stride_bytes == 192 && model.source_sync_offset_bytes == 4);
}

struct ProductEntry {
    const char *product_id;
    const ModelEntry *const *models;
    std::size_t model_count;
};

class ProductInstance {
public:
    ProductInstance(const ModelEntry &model, dtv_device_id device) : model_(&model)
    {
        if ((model_->create_state == nullptr) != (model_->destroy_state == nullptr)) {
            result_ = dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
            return;
        }
        if (model_->create_state) {
            dtv_context *temporary = nullptr;
            result_ = dtv::internal::call_callback([&] {
                return model_->create_state(device, &temporary);
            });
            if (result_.code == DTV_INTERNAL_RESULT_OK) {
                context_ = temporary;
            } else if (temporary != nullptr) {
                destroy(temporary);
            }
        }
    }

    ~ProductInstance() { reset(); }

    void reset() noexcept
    {
        auto *context = context_;
        context_ = nullptr;
        if (context && model_->destroy_state) {
            destroy(context);
        }
    }

    ProductInstance(const ProductInstance &) = delete;
    ProductInstance &operator=(const ProductInstance &) = delete;

    dtv_context *context() const { return context_; }
    dtv_internal_result result() const { return result_; }

private:
    void destroy(dtv_context *context) noexcept
    {
        if (model_->destroy_state) {
            try {
                model_->destroy_state(context);
            } catch (...) {
            }
        }
    }

    const ModelEntry *model_;
    dtv_context *context_ = nullptr;
    dtv_internal_result result_ = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
};

std::span<const ProductEntry *const> product_registry();

}

#endif
