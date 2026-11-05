/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_SPI_DEVICE_OPS_HPP
#define DTV_SPI_DEVICE_OPS_HPP

#include <cstddef>
#include <span>

#include <dtv/cli.h>
#include <dtv/spi/types.h>

namespace dtv::spi {

struct DeviceOps {
    dtv_result (*open)(dtv_context *, const dtv_usb_io *, const dtv_open_args *);
    dtv_result (*identify)(dtv_context *, dtv_identify_info *, dtv_timeout);
    dtv_result (*firmware)(dtv_context *, uint32_t, dtv_bytes, dtv_firmware_result *,
                           dtv_timeout);
    dtv_result (*tune_t)(dtv_context *, dtv_receiver, uint32_t, uint64_t,
                         dtv_tune_result *, dtv_timeout);
    dtv_result (*signal)(dtv_context *, dtv_receiver, dtv_signal_result *, dtv_timeout);
    dtv_result (*ts_prepare)(dtv_context *, dtv_receiver, uint64_t,
                             const dtv_stream_program *, dtv_timeout);
    dtv_result (*service_tick)(dtv_context *, const dtv_usb_io *, uint64_t, dtv_timeout);
    dtv_result (*ts_transform)(dtv_context *, dtv_receiver, dtv_bytes, dtv_buffer *);
    dtv_result (*ts_stop_device)(dtv_context *, dtv_receiver, dtv_timeout);
    dtv_result (*close)(dtv_context *, dtv_timeout);
    dtv_result (*card_uart_write_byte)(dtv_context *, uint8_t, dtv_timeout);
    dtv_result (*card_uart_read_byte)(dtv_context *, uint8_t *, dtv_timeout);
    dtv_result (*card_frame_write)(dtv_context *, dtv_bytes, dtv_timeout);
    dtv_result (*card_frame_read)(dtv_context *, dtv_buffer *, dtv_timeout);
};

struct FirmwareEntry {
    dtv_fw_key key;
    const DeviceOps *ops;
};

struct ModelEntry {
    dtv_model_info model;
    const FirmwareEntry *firmware;
    // Zero entries leave the model unbound; there is no default operations table.
    std::size_t firmware_count;
    dtv_result (*create_state)(dtv_device, dtv_context **);
    void (*destroy_state)(dtv_context *);
};

class ProductInstance {
public:
    ProductInstance(const ModelEntry &model, dtv_device device) : model_(&model)
    {
        if (model_->create_state) {
            result_ = model_->create_state(device, &context_);
        }
    }

    ~ProductInstance()
    {
        if (context_ && model_->destroy_state) {
            model_->destroy_state(context_);
        }
    }

    ProductInstance(const ProductInstance &) = delete;
    ProductInstance &operator=(const ProductInstance &) = delete;

    dtv_context *context() const { return context_; }
    dtv_result result() const { return result_; }

private:
    const ModelEntry *model_;
    dtv_context *context_ = nullptr;
    dtv_result result_{};
};

std::span<const ModelEntry *const> product_models();

}

#endif
