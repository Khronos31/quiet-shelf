// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>

namespace dtv::spi {

extern const ModelEntry fixture_two_model{
    .model = {"Fixture Two", 0x5678, 0x9abc},
    .firmware = nullptr,
    .firmware_count = 0,
};

}
