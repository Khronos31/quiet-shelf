/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PIX_DT295_INTERNAL_HPP
#define PIX_DT295_INTERNAL_HPP

#include <array>

#include <dtv/spi/device_ops.hpp>

namespace dtv::pix::detail {

enum class DeviceState {
    unknown,
    cold,
    runtime,
};

struct ReadOnlyObservation {
    uint16_t status = 0;
    bool guards_match = false;
    uint32_t source = 0;
    uint32_t pointer = 0;
    std::array<uint32_t, 2> main_marker{};
    std::array<uint32_t, 2> main_heartbeat{};
    std::array<uint32_t, 2> secondary_marker{};
    std::array<uint32_t, 2> secondary_heartbeat{};
};

struct State {
    dtv_device_id device;
    dtv_usb_io io{};
    dtv_timeout open_timeout{};
    bool opened = false;
    DeviceState device_state = DeviceState::unknown;
    bool prepared = false;
    bool lease = false;
    bool failed = false;
    bool stage_started = false;
    bool trigger_attempted = false;
    uint32_t failure_phase = 0;
    dtv_internal_result failure_result =
        dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
};

dtv_internal_result open(dtv_context *, const dtv_usb_io *, const dtv_open_args *);
dtv_internal_result identify(dtv_context *, dtv_identify_info *, dtv_timeout);
dtv_internal_result close(dtv_context *, dtv_timeout);
dtv_internal_result select_usb_group(const dtv_usb_candidate_info *, uint32_t, uint32_t,
                                     dtv_usb_function_selection *, uint32_t, uint32_t *,
                                     dtv_usb_group_match *);
dtv_internal_result firmware(dtv_context *, dtv_bytes, dtv_firmware_result *, dtv_timeout);
dtv_internal_result create_state(dtv_device_id, dtv_context **);
void destroy_state(dtv_context *);

}

#endif
