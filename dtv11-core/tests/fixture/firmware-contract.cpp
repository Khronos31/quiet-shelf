/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/internal/l3.hpp>
#include <assetfixture_api.h>

extern "C" void dtv_load_assetfixture(void);
int main()
{
    dtv_load_assetfixture();
    uint32_t support = DTV_UNKNOWN;
    if (dtv_get_model_support("Firmware Fixture", DTV_OP_LNB_POWER, &support, nullptr) !=
            DTV_ERROR_OK || support != DTV_NO) {
        return 1;
    }
    dtv_error error{};
    error.struct_size = sizeof(error);
    if (dtv_send_default_firmware(nullptr, nullptr, &error) != DTV_ERROR_INVALID_ARGUMENT ||
        error.code != DTV_ERROR_INVALID_ARGUMENT ||
        dtv_send_default_firmware_assetfixture(nullptr, nullptr, &error) !=
            DTV_ERROR_INVALID_ARGUMENT || error.code != DTV_ERROR_INVALID_ARGUMENT) {
        return 2;
    }
    return dtv::internal::dispatch_send_default_firmware(
               "assetfixture", "Firmware Fixture", nullptr, nullptr, nullptr) == DTV_ERROR_OK
               ? 0
               : 3;
}
