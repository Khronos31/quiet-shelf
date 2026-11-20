/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix_dt295_internal.hpp"

#include <iterator>

#include <pix_firmware.hpp>
#include <dtv/spi/firmware_session.hpp>

DTV_PRODUCT(pix, DTV_FEATURES());
DTV_MODEL(pix, pix_dt295_custom_v25, DTV_FEATURES(),
          DTV_FIRMWARE(pix_dt295_firmware),
          (::dtv::spi::model_information{"PIX-DT295", 0x06B8, 0x103B}));
DTV_DEFINE_MODEL(pix, pix_dt295_custom_v25);

namespace dtv::spi {

namespace {

constexpr dtv_function kReceiverFunction = 1;
constexpr DeviceOps pix_dt295_bootstrap_ops{
    .open = dtv::pix::detail::open,
    .identify = dtv::pix::detail::identify,
    .close = dtv::pix::detail::close,
};
constexpr DeviceOps pix_dt295_runtime_ops{};
constexpr FirmwareEntry pix_dt295_firmware[]{{25, &pix_dt295_runtime_ops}};
constexpr dtv_usb_identity pix_dt295_cold_identities[]{{0x06B8, 0x103B}};
constexpr FunctionEntry pix_dt295_functions[]{
    {
        .function = kReceiverFunction,
        .bootstrap_ops = &pix_dt295_bootstrap_ops,
        .firmware = pix_dt295_firmware,
        .firmware_count = std::size(pix_dt295_firmware),
        .create_state = dtv::pix::detail::create_state,
        .destroy_state = dtv::pix::detail::destroy_state,
    },
};

}

extern const ModelEntry pix_dt295_custom_v25_model{
    .model = {"PIX-DT295", 0x06B8, 0x103B},
    .firmware = pix_dt295_firmware,
    .firmware_count = std::size(pix_dt295_firmware),
    .create_state = dtv::pix::detail::create_state,
    .destroy_state = dtv::pix::detail::destroy_state,
    .bootstrap_ops = &pix_dt295_bootstrap_ops,
    .cold_identities = pix_dt295_cold_identities,
    .cold_identity_count = std::size(pix_dt295_cold_identities),
    .select_usb_group = dtv::pix::detail::select_usb_group,
    .functions = pix_dt295_functions,
    .function_count = std::size(pix_dt295_functions),
};

}

namespace {

int32_t public_error(dtv_internal_result result)
{
    switch (result.code) {
    case DTV_INTERNAL_RESULT_OK: return DTV_ERROR_OK;
    case DTV_INTERNAL_RESULT_INVALID_ARGUMENT: return DTV_ERROR_INVALID_ARGUMENT;
    case DTV_INTERNAL_RESULT_UNSUPPORTED: return DTV_ERROR_UNSUPPORTED;
    case DTV_INTERNAL_RESULT_USB_IO: return DTV_ERROR_USB_IO;
    case DTV_INTERNAL_RESULT_TIMEOUT: return DTV_ERROR_TIMEOUT;
    case DTV_INTERNAL_RESULT_NOT_READY: return DTV_ERROR_NOT_READY;
    case DTV_INTERNAL_RESULT_DISCONNECTED: return DTV_ERROR_DISCONNECTED;
    case DTV_INTERNAL_RESULT_CANCELLED: return DTV_ERROR_CANCELLED;
    default: return DTV_ERROR_INTERNAL;
    }
}

int32_t transfer_firmware(dtv_context *context,
                          const dtv::spi::FirmwareSession *session,
                          dtv_bytes package, dtv_error *error)
{
    if (context == nullptr || session == nullptr || session->io == nullptr) {
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    auto *state = static_cast<dtv::pix::detail::State *>(context);
    state->io = *session->io;
    dtv_firmware_result result{};
    const auto transferred = dtv::pix::detail::firmware(
        context, package, &result, state->open_timeout);
    const auto code = public_error(transferred);
    if (error != nullptr && error->struct_size >= sizeof(error->code)) {
        error->code = code;
    }
    return code;
}

}

extern "C" int32_t dtv_model_impl_send_firmware_pix_pix_dt295_custom_v25(
    dtv_device *device, const uint8_t *firmware, uint64_t size,
    const dtv_call_options *options, dtv_error *error)
{
    return dtv::spi::with_firmware_session(
        device, {firmware, size}, options, transfer_firmware, error);
}
