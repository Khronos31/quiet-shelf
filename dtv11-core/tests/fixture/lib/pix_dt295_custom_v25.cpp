// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>

namespace dtv::spi {

extern const ModelEntry pix_dt295_custom_v25_model{
    .model = {"PIX-DT295", 0x06b8, 0x103b},
    .firmware = nullptr,
    .firmware_count = 0,
    .source_stride_bytes = 188,
    .source_sync_offset_bytes = 0,
};

}
