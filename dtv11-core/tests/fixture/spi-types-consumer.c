/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/spi/types.h>

_Static_assert(DTV_INTERNAL_RESULT_OK == 0, "SPI result values are fixed");
_Static_assert(DTV_INTERNAL_RESULT_INVALID_ARGUMENT == 2, "SPI result values are fixed");
_Static_assert(DTV_INTERNAL_RESULT_UNSUPPORTED == 3, "SPI result values are fixed");
_Static_assert(DTV_INTERNAL_RESULT_USB_IO == 7, "SPI result values are fixed");
_Static_assert(DTV_INTERNAL_RESULT_TIMEOUT == 10, "SPI result values are fixed");

dtv_internal_result spi_types_c_consumer(dtv_bytes bytes, dtv_timeout timeout)
{
    dtv_internal_result result;
    dtv_usb_candidate_info candidate = {0};
    dtv_usb_function_selection selection = {0};
    dtv_select_usb_group_fn selector = 0;
    (void)bytes;
    (void)timeout;
    (void)candidate;
    (void)selection;
    (void)selector;
    result.code = DTV_INTERNAL_RESULT_OK;
    result.detail = 0;
    return result;
}
