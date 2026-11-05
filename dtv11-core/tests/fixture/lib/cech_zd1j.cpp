// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>

namespace dtv::spi {

extern constexpr DeviceOps cech_zd1j_ops{};

extern const ModelEntry cech_zd1j_model{
    .model = {"CECH-ZD1J", 0x054c, 0x04b2},
    .firmware = nullptr,
    .firmware_count = 0,
};

}
