/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstddef>

#include "torne_api.h"

extern "C" int32_t dtv_send_default_firmware_torne_cech_zd1j(
    dtv_device *, const dtv_call_options *, dtv_error *);

extern "C" int32_t dtv_send_default_firmware_to_all_torne(
    const dtv_call_options *options, dtv_error *error)
{
    auto *const *enclosures = dtv_list_enclosures_torne(error);
    if (enclosures == nullptr) return error == nullptr ? DTV_ERROR_INTERNAL : error->code;
    for (std::size_t enclosure_index = 0; enclosures[enclosure_index] != nullptr;
         ++enclosure_index) {
        auto *const *devices = dtv_get_devices(enclosures[enclosure_index]);
        if (devices == nullptr) continue;
        for (std::size_t device_index = 0; devices[device_index] != nullptr; ++device_index) {
            const auto sent = dtv_send_default_firmware_torne_cech_zd1j(
                devices[device_index], options, error);
            if (sent != DTV_ERROR_OK) return sent;
        }
    }
    return DTV_ERROR_OK;
}
