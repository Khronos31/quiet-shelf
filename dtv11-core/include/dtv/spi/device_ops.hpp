/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_SPI_DEVICE_OPS_HPP
#define DTV_SPI_DEVICE_OPS_HPP

#include <span>

#include <dtv/cli.h>

namespace dtv::spi {

struct DeviceOps {
    dtv_model_info model;
    void (*open)(void *) = nullptr;
    void (*close)(void *) = nullptr;
};

std::span<const DeviceOps> product_devices();

}

#endif
