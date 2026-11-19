/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_WIRE_STATUS_RESPONSE_HPP
#define DTV_INTERNAL_WIRE_STATUS_RESPONSE_HPP

#include <cstddef>
#include <cstdint>

#include <dtv/internal/wire-list-response.hpp>
#include <dtv/internal/wire-status-request.hpp>

namespace dtv::internal {

struct StatusContext {
    std::array<std::uint8_t, 16> epoch{};
    WireOptional<WireText<64>> instance;
    std::uint8_t state = 0;
    WireOptional<ListErrorRecord> last_error;
};

struct StatusTune {
    std::uint64_t revision = 0;
    std::uint8_t system = 0;
    std::uint64_t frequency_hz = 0;
    WireOptional<std::uint64_t> programmed_hz;
    std::uint8_t selection = 0;
    std::uint16_t selection_value = 0;
    std::uint8_t verification = 0;
};

struct StatusStream {
    std::uint64_t id = 0;
    std::uint64_t tune_revision = 0;
    std::uint8_t phase = 0;
};

struct StatusCounterValue {
    std::uint8_t state = 0;
    std::uint8_t reason = 0;
    std::uint64_t value = 0;
    std::uint64_t sampled_at_ns = 0;
    bool overflow = false;
};

struct StatusCounterEntry {
    std::uint8_t id = 0;
    StatusCounterValue counter;
};

struct StatusStreamCounters {
    WireArray<StatusCounterEntry, 16> entries;
};

struct StatusReceiverCounters {
    WireArray<StatusCounterEntry, 4> entries;
};

struct StatusStreamFinal {
    std::uint64_t id = 0;
    std::uint64_t tune_revision = 0;
    std::uint8_t reason = 0;
    WireOptional<ListErrorRecord> error;
    StatusStreamCounters statistics;
};

struct StatusMeasurement {
    std::uint8_t state = 0;
    std::uint8_t reason = 0;
    bool boolean_value = false;
    std::int32_t integer_value = 0;
    std::uint64_t sampled_at_ns = 0;
};

struct StatusMetricEntry {
    std::uint8_t id = 0;
    StatusMeasurement measurement;
};

struct StatusMeasurements {
    WireArray<StatusMetricEntry, 3> entries;
};

struct StatusReceiver {
    ListReceiver descriptor;
    bool present = false;
    std::uint8_t state = 0;
    std::uint8_t operation = 0;
    bool leased = false;
    std::uint64_t tune_revision = 0;
    WireOptional<StatusTune> current_tune;
    WireOptional<StatusTune> last_successful_tune;
    WireOptional<StatusStream> stream;
    WireOptional<StatusStreamFinal> last_stream;
    StatusMeasurements measurements;
    StatusReceiverCounters receiver_statistics;
    WireOptional<StatusStreamCounters> stream_statistics;
    WireOptional<ListErrorRecord> last_error;
};

struct StatusCardObservation {
    std::uint8_t state = 0;
    WireOptional<std::uint64_t> sampled_at_ns;
    std::uint8_t reason = 0;
};

struct StatusReader {
    ListReader descriptor;
    bool present = false;
    StatusCardObservation card;
    std::uint8_t operation = 0;
    std::uint16_t handles_open = 0;
    bool transaction_active = false;
    WireOptional<ListErrorRecord> last_error;
};

struct StatusResponse {
    ListProducer producer;
    StatusContext context;
    ListSnapshot snapshot;
    Scope scope;
    WireOptional<ListEnclosure> enclosure;
    WireOptional<bool> lnb_15v_enabled;
    WireOptional<bool> card_inserted;
    ListTopology topology;
    WireOptional<WireArray<ListUsbFunction, 8>> functions;
    WireOptional<WireArray<StatusReceiver, 32>> receivers;
    WireOptional<WireArray<StatusReader, 8>> readers;
};

WireFrameCode status_response_payload_size(const StatusResponse &value, std::size_t *size);
WireFrameCode encode_status_response(const StatusResponse &value, dtv_buffer *output);
WireFrameCode decode_status_response(dtv_bytes payload, StatusResponse *value);
WireFrameCode validate_status_response_scope(const StatusResponse &value);
WireFrameCode validate_status_response(const StatusResponse &value, const ListResponse &list);

}

#endif
