/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/spi/types.h>

dtv_result spi_types_c_consumer(dtv_bytes bytes, dtv_timeout timeout)
{
    dtv_result result;
    (void)bytes;
    (void)timeout;
    result.code = 0;
    result.detail = 0;
    return result;
}
