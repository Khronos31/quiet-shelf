/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_WIRE_LIST_RESPONSE_HPP
#define DTV_INTERNAL_WIRE_LIST_RESPONSE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include <dtv/internal/wire-envelope.hpp>

namespace dtv::internal {

template <std::size_t N> struct WireText {
    std::array<std::uint8_t, N> bytes{};
    std::uint16_t length = 0;
};
template <typename T> struct WireOptional { bool present = false; T value{}; };
template <typename T, std::size_t N> struct WireArray {
    std::array<T, N> values{};
    std::uint16_t count = 0;
};

struct ListModel { WireText<128> name; };
struct ListErrorRecord { std::uint32_t code = 0; WireText<32> operation; WireText<256> detail; };
struct ListLocation { WireOptional<WireText<64>> usb_path; WireOptional<std::uint8_t> bus; WireOptional<std::uint8_t> address; };
struct ListUsbFunction {
    std::uint16_t id = 0, vid = 0, pid = 0;
    std::uint8_t mode = 0;
    ListLocation location;
    std::uint8_t serial_state = 0;
    WireOptional<WireText<512>> serial;
    WireOptional<ListModel> model;
    std::uint8_t identification_reason = 0;
    bool present = false;
    WireOptional<ListErrorRecord> last_error;
};
struct ListBinding { std::uint16_t function_id = 0, local_id = 0; };
struct ListLnaDefault { std::uint8_t system = 0; WireOptional<bool> enabled; };
struct ListLnbValue { std::uint8_t value = 0; };
struct ListCapabilities {
    std::uint8_t ts_scope = 0;
    WireOptional<std::uint16_t> max_pids;
    std::uint8_t controls = 0;
    WireArray<ListLnaDefault, 2> lna_defaults;
    WireArray<ListLnbValue, 2> lnb_values;
    std::uint8_t measurement_mask = 0;
};
struct ListReceiver { std::uint16_t id = 0; std::uint8_t systems = 0; WireOptional<ListBinding> binding; ListCapabilities capabilities; };
struct ListReader { std::uint16_t id = 0; WireOptional<ListBinding> binding; };
struct ListIdentity { std::uint8_t serial_state = 0; WireOptional<WireText<512>> serial; std::uint8_t uniqueness = 0; };
struct ListLimits { WireOptional<std::uint16_t> max_concurrent_streams; };
struct ListEnclosure { std::uint16_t id = 0; WireOptional<ListModel> model; ListIdentity identity; ListLimits limits; };
struct ListProducer { WireText<32> product; WireText<64> version; WireOptional<WireText<64>> core_version; };
struct ListSnapshot { std::uint64_t sequence = 0, observed_at_ns = 0; };
struct ListTopology { std::uint8_t state = 0; WireOptional<std::uint16_t> expected_function_count; };
struct ListResponse {
    std::array<std::uint8_t, 16> epoch{};
    ListProducer producer;
    ListSnapshot snapshot;
    ListTopology topology;
    WireOptional<ListEnclosure> enclosure;
    WireArray<ListUsbFunction, 8> functions;
    WireOptional<WireArray<ListReceiver, 32>> receivers;
    WireOptional<WireArray<ListReader, 8>> readers;
};

WireFrameCode list_response_payload_size(const ListResponse &value, std::size_t *size);
WireFrameCode encode_list_response(const ListResponse &value, dtv_buffer *output);
WireFrameCode decode_list_response(dtv_bytes payload, ListResponse *value);
WireFrameCode validate_list_response(const ListResponse &value);

}

#endif
