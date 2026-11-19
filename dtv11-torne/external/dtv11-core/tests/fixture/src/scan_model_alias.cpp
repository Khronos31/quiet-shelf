/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

namespace dtv::spi {

extern const ModelEntry scan_model_alias_model{
    .model = {"Scan Fixture Alias", 0x1234, 0x0201},
    .firmware = nullptr,
    .firmware_count = 0,
};

}

DTV_PRODUCT(scanfixture, DTV_FEATURES());
inline constexpr dtv::spi::model_information::device_receiver_information scan_alias_device_receivers[]{
    {1, true, nullptr, 0},
};
DTV_MODEL(scanfixture, scan_model_alias, DTV_FEATURES(), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"Scan Fixture Alias", 0x1234, 0x0201,
                                           scan_alias_device_receivers,
                                           std::size(scan_alias_device_receivers)}));
DTV_DEFINE_MODEL(scanfixture, scan_model_alias);
