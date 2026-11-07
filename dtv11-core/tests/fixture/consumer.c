/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/cli.h>

#include <string.h>

_Static_assert(DTV_ERROR_OK == 0, "L3 error values are fixed");
_Static_assert(DTV_ERROR_INVALID_ARGUMENT == 1, "L3 error values are fixed");
_Static_assert(DTV_ERROR_NOT_FOUND == 2, "L3 error values are fixed");
_Static_assert(DTV_ERROR_NOT_READY == 3, "L3 error values are fixed");
_Static_assert(DTV_ERROR_UNSUPPORTED == 4, "L3 error values are fixed");
_Static_assert(DTV_ERROR_BUSY == 5, "L3 error values are fixed");
_Static_assert(DTV_ERROR_TIMEOUT == 6, "L3 error values are fixed");
_Static_assert(DTV_ERROR_VERSION_MISMATCH == 7, "L3 error values are fixed");
_Static_assert(DTV_ERROR_PROTOCOL_ERROR == 8, "L3 error values are fixed");
_Static_assert(DTV_ERROR_USB_IO == 9, "L3 error values are fixed");
_Static_assert(DTV_ERROR_DISCONNECTED == 10, "L3 error values are fixed");
_Static_assert(DTV_ERROR_TS_ERROR == 11, "L3 error values are fixed");
_Static_assert(DTV_ERROR_NO_CARD == 12, "L3 error values are fixed");
_Static_assert(DTV_ERROR_CARD_REMOVED == 13, "L3 error values are fixed");
_Static_assert(DTV_ERROR_CARD_IO == 14, "L3 error values are fixed");
_Static_assert(DTV_ERROR_BUFFER_TOO_SMALL == 15, "L3 error values are fixed");
_Static_assert(DTV_ERROR_FIRMWARE_INVALID == 16, "L3 error values are fixed");
_Static_assert(DTV_ERROR_PERMISSION_DENIED == 17, "L3 error values are fixed");
_Static_assert(DTV_ERROR_RESOURCE_LIMIT == 18, "L3 error values are fixed");
_Static_assert(DTV_ERROR_OUTPUT_ERROR == 19, "L3 error values are fixed");
_Static_assert(DTV_ERROR_INTERNAL == 20, "L3 error values are fixed");

int main(void)
{
    const char *argv[] = {"consumer", "--version"};
    const dtv_cli_config config = {sizeof(dtv_cli_config), "consumer", "consumer", "1.0.0", ""};
    uint32_t model_count = 0;
    dtv_model_info model = {0};
    if (dtv_api_version() != UINT32_C(0x00020000)) {
        return 1;
    }
    if (dtv_model_count(&model_count).code != DTV_ERROR_OK || model_count != 2) {
        return 2;
    }
    if (dtv_model_get(0, &model).code != DTV_ERROR_OK ||
        strcmp(model.name, "Fixture One") != 0 || model.vendor_id != 0x1234 ||
        model.product_id != 0x0001) {
        return 3;
    }
    if (dtv_model_get(model_count, &model).code != DTV_ERROR_INVALID_ARGUMENT) {
        return 4;
    }
    return dtv_cli_main(2, argv, &config, 0, 0);
}
