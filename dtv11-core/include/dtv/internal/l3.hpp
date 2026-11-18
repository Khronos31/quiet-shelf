/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_L3_HPP
#define DTV_INTERNAL_L3_HPP

#include <dtv/dtv.h>

#include <cstdint>
#include <vector>

namespace dtv::internal {

dtv_enclosure *const *list_enclosures_for_product(const char *, dtv_error *);
dtv_enclosure *const *list_enclosures_for_product_by_tokens(
    const char *, const dtv_usb_token *, uint32_t, dtv_error *);
int32_t dispatch_lnb_on(const char *, const char *, dtv_enclosure *, const dtv_call_options *, dtv_error *);
int32_t dispatch_lnb_off(const char *, const char *, dtv_enclosure *, const dtv_call_options *, dtv_error *);
int32_t dispatch_tune_by_frequency(const char *, const char *, dtv_receiver *, uint32_t, uint64_t,
                                   const dtv_tune_options *, const dtv_call_options *, dtv_error *);
int32_t dispatch_get_signal(const char *, const char *, dtv_receiver *, const dtv_call_options *,
                            dtv_signal *, dtv_error *);
int32_t dispatch_set_pid_filter(const char *, const char *, dtv_stream *, const uint16_t *, uint32_t,
                                dtv_error *);
int32_t dispatch_send_firmware(const char *, const char *, dtv_device *, const uint8_t *, uint64_t,
                               const dtv_call_options *, dtv_error *);
int32_t dispatch_send_default_firmware(const char *, const char *, dtv_device *,
                                       const dtv_call_options *, dtv_error *);
int32_t dispatch_reset_card(const char *, const char *, dtv_enclosure *, const dtv_call_options *,
                            std::vector<std::uint8_t> *, dtv_error *);

}

#endif
