// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

namespace dtv::spi {

extern constexpr DeviceOps cech_zd1j_ops{};

extern const ModelEntry cech_zd1j_model{
    .model = {"CECH-ZD1J", 0x054c, 0x04b2},
    .firmware = nullptr,
    .firmware_count = 0,
    .source_stride_bytes = 192,
    .source_sync_offset_bytes = 4,
};

}

DTV_PRODUCT(i0_torne_fixture, DTV_FEATURES());
DTV_MODEL(i0_torne_fixture, cech_zd1j, DTV_FEATURES(), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"CECH-ZD1J", 0x054c, 0x04b2}));
DTV_DEFINE_MODEL(i0_torne_fixture, cech_zd1j);
