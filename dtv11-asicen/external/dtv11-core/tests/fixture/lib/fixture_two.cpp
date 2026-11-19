// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

namespace dtv::spi {

extern const ModelEntry fixture_two_model{
    .model = {"Fixture Two", 0x5678, 0x9abc},
    .firmware = nullptr,
    .firmware_count = 0,
};

}

DTV_PRODUCT(fixture, DTV_FEATURES(::dtv::spi::Feature::LnbPower));
DTV_MODEL(fixture, fixture_two, DTV_FEATURES(), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"Fixture Two", 0x5678, 0x9abc}));
DTV_DEFINE_MODEL(fixture, fixture_two);
