// SPDX-License-Identifier: GPL-2.0-or-later
#include "sample_decl.hpp"

#include <cstdio>

dtv::link_spike::Result dtv::link_spike::sample_model_send_firmware(
    const std::uint8_t *data, std::size_t size)
{
    if (data == nullptr || size == 0) {
        return Result::error;
    }
    std::printf("handwritten firmware function: bytes=%zu first=0x%02x\n",
                size, static_cast<unsigned int>(data[0]));
    return Result::ok;
}
