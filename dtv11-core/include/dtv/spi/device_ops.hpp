/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_SPI_DEVICE_OPS_HPP
#define DTV_SPI_DEVICE_OPS_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include <dtv/cli.h>
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
};

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
            result_ = {DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0};
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
    dtv_internal_result result_{};
};

std::span<const ProductEntry *const> product_registry();

}

#endif
