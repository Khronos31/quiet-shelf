/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstring>

#include <pix_api.h>

extern "C" int32_t dtv_send_default_firmware_pix_pix_dt295_custom_v25(
    dtv_device *, const dtv_call_options *, dtv_error *);

extern "C" int32_t dtv_send_default_firmware_to_all_pix(
    const dtv_call_options *options, dtv_error *error)
{
    auto *const *enclosures = dtv_list_enclosures_pix(error);
    if (enclosures == nullptr) {
        return error != nullptr && error->struct_size >= sizeof(error->code)
            ? error->code : DTV_ERROR_INTERNAL;
    }
    for (auto *const *entry = enclosures; *entry != nullptr; ++entry) {
        const auto *model = dtv_get_model(*entry);
        if (model == nullptr || std::strcmp(model, "PIX-DT295") != 0) continue;
        for (auto *const *device = dtv_get_devices(*entry); *device != nullptr; ++device) {
            const auto result = dtv_send_default_firmware_pix_pix_dt295_custom_v25(
                *device, options, error);
            if (result != DTV_ERROR_OK) return result;
        }
    }
    return DTV_ERROR_OK;
}
