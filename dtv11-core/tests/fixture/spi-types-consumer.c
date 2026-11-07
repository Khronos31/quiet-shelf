/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/spi/types.h>

_Static_assert(DTV_INTERNAL_RESULT_OK == 0, "SPI result values are fixed");
_Static_assert(DTV_INTERNAL_RESULT_INVALID_ARGUMENT == 2, "SPI result values are fixed");
_Static_assert(DTV_INTERNAL_RESULT_UNSUPPORTED == 3, "SPI result values are fixed");
_Static_assert(DTV_INTERNAL_RESULT_USB_IO == 7, "SPI result values are fixed");
_Static_assert(DTV_INTERNAL_RESULT_TIMEOUT == 10, "SPI result values are fixed");
_Static_assert(DTV_USB_GROUP_INCOMPLETE == 3, "group match values are fixed");

dtv_internal_result spi_types_c_consumer(dtv_bytes bytes, dtv_timeout timeout)
{
    dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
    static const char diagnostic[] = "C callback diagnostic";
    dtv_usb_candidate_info candidate = {0};
    dtv_usb_function_selection selection = {0};
    dtv_select_usb_group_fn selector = 0;
    struct dtv_open_args open_args = {0};
    dtv_usb_candidate_info candidate_info = {0};
    (void)bytes;
    (void)timeout;
    (void)candidate;
    (void)selection;
    (void)selector;
    open_args.usb_observation = &candidate_info;
    (void)open_args;
    dtv_internal_result_set_diagnostic(&result, diagnostic, sizeof(diagnostic) - 1);
    return result;
}
