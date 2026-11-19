// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

namespace dtv::spi {

extern constexpr DeviceOps px_w3u3_ops{};

extern const ModelEntry px_w3u3_model{
    .model = {"PX-W3U3", 0x0b06, 0x0005},
    .firmware = nullptr,
    .firmware_count = 0,
    .source_stride_bytes = 188,
    .source_sync_offset_bytes = 0,
};

}

DTV_PRODUCT(i0_asicen_fixture, DTV_FEATURES());
DTV_MODEL(i0_asicen_fixture, px_w3u3, DTV_FEATURES(), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"PX-W3U3", 0x0b06, 0x0005}));
DTV_DEFINE_MODEL(i0_asicen_fixture, px_w3u3);
