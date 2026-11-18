/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_DISCOVERY_JSON_HPP
#define DTV_INTERNAL_DISCOVERY_JSON_HPP

#include <cstddef>
#include <cstdint>

#include <dtv/types.h>

namespace dtv::internal {

enum class DiscoveryJsonCode {
    ok,
    invalid_argument,
    buffer_too_small
};

/* 位置情報は呼出し中だけ借用する。NULL は JSON null。 */
struct DiscoveryJsonLocation {
    const std::uint32_t *bus = nullptr;
    const std::uint32_t *address = nullptr;
    const char *port = nullptr;
};

struct DiscoveryJsonReceiver {
    std::uint32_t receiver = 0;
    std::uint32_t device = 0;
    std::uint32_t local = 0;
    std::uint32_t systems = 0;
    std::uint32_t ts_scrambling = 0;
};

struct DiscoveryJsonCandidate {
    std::uint32_t device = 0;
    const char *serial = nullptr;
    DiscoveryJsonLocation location;
};

struct DiscoveryJsonDevice {
    std::uint32_t id = 0;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    const char *serial = nullptr;
    DiscoveryJsonLocation location;
    /* false は構成未知（鍵を省略）、true は既知（0 件なら []）。 */
    bool receivers_known = false;
    const DiscoveryJsonReceiver *receivers = nullptr;
    std::size_t receiver_count = 0;
};

struct DiscoveryJsonEnclosure {
    std::uint32_t id = 0;
    const char *model = nullptr;
    const char *serial = nullptr;
    std::uint32_t serial_unique = 0; /* dtv_tribool */
    std::uint32_t status = 0;        /* dtv_enclosure_status */
    std::uint32_t lnb_15v_supported = 0;
    std::uint32_t card_reader_supported = 0;
    bool has_card_reader_device = false;
    std::uint32_t card_reader_device = 0;
    const DiscoveryJsonDevice *devices = nullptr;
    std::size_t device_count = 0;
    const DiscoveryJsonCandidate *candidates = nullptr;
    std::size_t candidate_count = 0;
};

struct DiscoveryJsonUngrouped {
    const char *serial = nullptr;
    const char *model = nullptr;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::uint32_t status = 0; /* dtv_ungrouped_status */
    DiscoveryJsonLocation location;
};

struct DiscoveryJsonInput {
    const char *product = nullptr;
    const char *version = nullptr;
    bool complete = false;
    const DiscoveryJsonEnclosure *enclosures = nullptr;
    std::size_t enclosure_count = 0;
    const DiscoveryJsonUngrouped *ungrouped = nullptr;
    std::size_t ungrouped_count = 0;
};

/* validate -> 改行込み size -> 容量 -> 書込み。失敗時は出力先を変更しない。 */
DiscoveryJsonCode discovery_json_size(const DiscoveryJsonInput *input,
                                      std::uint64_t *bytes) noexcept;
DiscoveryJsonCode encode_discovery_json(const DiscoveryJsonInput *input,
                                        dtv_buffer *output) noexcept;

}

#endif
