/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/dtv.h>

extern "C" int32_t dtv_send_default_firmware_to_all_scanfixture(
    const dtv_call_options *, dtv_error *)
{
    return DTV_ERROR_OK;
}
