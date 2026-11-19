/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_SPI_FIRMWARE_SESSION_HPP
#define DTV_SPI_FIRMWARE_SESSION_HPP

#include <cstddef>
#include <cstdint>

#include <dtv/dtv.h>
#include <dtv/internal/result.hpp>
#include <dtv/spi/types.h>

namespace dtv::spi {

struct FirmwareSession {
    const dtv_usb_io *io;
    dtv_device_id device_id;
    dtv_function function;
    std::uint64_t connection_epoch;
    std::uint32_t initial_mode;
    dtv_identify_info initial_identity;
    std::size_t handle_index;
};

using FirmwarePackageTransfer = int32_t (*)(dtv_context *, const FirmwareSession *, dtv_bytes,
                                            dtv_error *);

int32_t with_firmware_session(dtv_device *, dtv_bytes, const dtv_call_options *,
                              FirmwarePackageTransfer, dtv_error *);

}

#endif
