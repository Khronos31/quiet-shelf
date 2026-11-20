// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

namespace dtv::spi {

extern const ModelEntry pix_dt295_custom_v25_model{
    .model = {"PIX-DT295", 0x06b8, 0x103b},
    .firmware = nullptr,
    .firmware_count = 0,
    .source_stride_bytes = 188,
    .source_sync_offset_bytes = 0,
};

}

DTV_PRODUCT(i0_pix_fixture, DTV_FEATURES());
DTV_MODEL(i0_pix_fixture, pix_dt295_custom_v25, DTV_FEATURES(), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"PIX-DT295", 0x06b8, 0x103b}));
DTV_DEFINE_MODEL(i0_pix_fixture, pix_dt295_custom_v25);
