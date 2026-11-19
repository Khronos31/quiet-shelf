// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/wire-lnb-power.hpp>

#include <algorithm>

namespace dtv::internal {
namespace {

bool view_valid(dtv_bytes view)
{
    return view.size == 0 || view.data != nullptr;
}

bool output_valid(const dtv_buffer *output)
{
    return output != nullptr && (output->capacity == 0 || output->data != nullptr);
}

}

WireFrameCode decode_lnb_power(dtv_bytes payload, LnbPower *value)
{
    if (value == nullptr) return WireFrameCode::invalid_argument;
    if (!view_valid(payload)) return WireFrameCode::invalid_argument;
    if (payload.size != kLnbPowerPayloadSize) return WireFrameCode::protocol_error;
    const auto enabled = payload.data[16];
    if (enabled != 0 && enabled != 1) return WireFrameCode::invalid_argument;
    LnbPower decoded;
    std::copy_n(payload.data, decoded.epoch.size(), decoded.epoch.begin());
    decoded.enabled = enabled != 0;
    *value = decoded;
    return WireFrameCode::ok;
}

WireFrameCode encode_lnb_power(const LnbPower &value, dtv_buffer *output)
{
    if (!output_valid(output)) return WireFrameCode::invalid_argument;
    if (output->capacity < kLnbPowerPayloadSize) return WireFrameCode::buffer_too_small;
    std::copy_n(value.epoch.begin(), value.epoch.size(), output->data);
    output->data[16] = value.enabled ? 1 : 0;
    output->size = kLnbPowerPayloadSize;
    return WireFrameCode::ok;
}

}
