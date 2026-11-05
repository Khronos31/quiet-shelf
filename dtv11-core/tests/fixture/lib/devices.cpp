// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstddef>

#include <dtv/spi/device_ops.hpp>

namespace dtv::spi {

std::span<const DeviceOps> product_devices()
{
    static constexpr std::array<DeviceOps, 2> devices{{
        {{"Fixture One", 0x1234, 0x0001}, nullptr, nullptr},
        {{"Fixture Two", 0x5678, 0x9abc}, nullptr, nullptr},
    }};
    return devices;
}

}

extern "C" size_t dtv_product_model_count(void)
{
    return dtv::spi::product_devices().size();
}

extern "C" const dtv_model_info *dtv_product_model_at(size_t index)
{
    const auto devices = dtv::spi::product_devices();
    return index < devices.size() ? &devices[index].model : nullptr;
}
