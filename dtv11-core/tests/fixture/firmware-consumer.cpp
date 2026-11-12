/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/spi/feature.hpp>
#include "fixture_firmware.hpp"

using fixture_firmware = DTV_FIRMWARE(fixture_blob);
static_assert(fixture_firmware::asset_type::size == 4);
static_assert(fixture_firmware::asset_type::data[0] == 0x12);

int fixture_firmware_consumer()
{
    return static_cast<int>(fixture_firmware::asset_type::size);
}
