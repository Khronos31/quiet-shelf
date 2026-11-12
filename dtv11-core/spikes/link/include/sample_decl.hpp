// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "link_spike.hpp"

DTV_SPIKE_PRODUCT(sample, dtv::link_spike::Feature::send_firmware);
DTV_SPIKE_MODEL(sample, sample_model,
                dtv::link_spike::Feature::send_firmware,
                DTV_SPIKE_FIRMWARE(sample_fw));

namespace dtv::link_spike {
Result sample_model_send_firmware(const std::uint8_t *data, std::size_t size);
Result sample_model_get_signal();
}
