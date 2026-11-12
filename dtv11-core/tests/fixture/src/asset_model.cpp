/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>
#include "assetfixture_firmware.hpp"

DTV_PRODUCT(assetfixture, DTV_FEATURES(::dtv::spi::Feature::Firmware));
DTV_MODEL(assetfixture, asset_model,
          DTV_FEATURES(::dtv::spi::Feature::Firmware),
          DTV_FIRMWARE(fixture_blob),
          (::dtv::spi::model_information{"Firmware Fixture", 0x1234, 0x5678}));
DTV_DEFINE_MODEL(assetfixture, asset_model);

extern "C" int32_t dtv_model_impl_send_firmware_assetfixture_asset_model(
    dtv_device *, const uint8_t *firmware, uint64_t size,
    const dtv_call_options *, dtv_error *)
{
    return size == dtv::spi::firmware_assets::fixture_blob::size &&
                   firmware[0] == dtv::spi::firmware_assets::fixture_blob::data[0] &&
                   firmware[size - 1] == dtv::spi::firmware_assets::fixture_blob::data[size - 1]
               ? DTV_ERROR_OK
               : DTV_ERROR_FIRMWARE_REJECTED;
}

namespace dtv::spi {

extern const ModelEntry asset_model_model{
    .model = {"Firmware Fixture", 0x1234, 0x5678},
    .firmware = nullptr,
    .firmware_count = 0,
};

}
