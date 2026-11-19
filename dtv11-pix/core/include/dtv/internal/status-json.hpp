/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_STATUS_JSON_HPP
#define DTV_INTERNAL_STATUS_JSON_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include <dtv/types.h>

namespace dtv::internal {

enum class StatusJsonCode {
    ok,
    invalid_argument,
    buffer_too_small
};

/* 呼出し中だけ有効な借用文字列。present=false は JSON の null/省略を表す。 */
struct StatusText {
    const char *data = nullptr;
    std::size_t size = 0;
    bool present = false;
};

template <typename T>
struct StatusArray {
    const T *items = nullptr;
    std::size_t count = 0;
    bool present = false;
};

template <typename T>
struct StatusOptional {
    bool present = false;
    T value{};
};

struct StatusBinding {
    bool present = false;
    std::uint16_t function_id = 0;
    std::uint16_t local_id = 0;
};

struct StatusLimits {
    StatusOptional<std::uint16_t> max_concurrent_streams;
};

struct StatusIdentity {
    std::uint8_t serial_state = 0; /* 0 unknown, 1 present, 2 absent */
    StatusText serial;
    std::uint8_t uniqueness = 0; /* 0 unknown, 1 unique_in_scan, 2 duplicate, 3 not_applicable */
};

struct StatusTopology {
    std::uint8_t state = 0; /* 0 unknown, 1 partial, 2 complete */
    StatusOptional<std::uint16_t> expected_function_count;
};

struct StatusLocation {
    StatusText usb_path;
    StatusOptional<std::uint8_t> bus;
    StatusOptional<std::uint8_t> address;
};

struct StatusError {
    bool present = false;
    std::uint32_t code = 0;
    StatusText operation;
    StatusText detail;
};

struct StatusProducerView {
    StatusText product;
    StatusText version;
    StatusText core_version;
};

struct StatusLnaEntry {
    std::uint8_t system = 1; /* 1 isdb_t, 2 isdb_s */
    StatusOptional<bool> default_value;
};

struct StatusLna {
    std::array<StatusLnaEntry, 2> entries{};
    std::size_t count = 0;
};

struct StatusLnbValues {
    std::array<std::uint8_t, 2> values{};
    std::size_t count = 0;
};

struct StatusCapabilities {
    std::uint8_t ts_scope = 0; /* 0 multiplex, 1 pid_subset */
    StatusOptional<std::uint16_t> max_pids;
    std::uint8_t controls = 0; /* bit0 lna, bit1 lnb */
    StatusLna lna;
    StatusLnbValues lnb;
    std::uint8_t measurement_mask = 0;
};

struct StatusReceiverDescriptor {
    std::uint16_t receiver = 0;
    std::uint8_t systems = 0; /* bit0 isdb_t, bit1 isdb_s */
    StatusBinding binding;
    StatusCapabilities capabilities;
};

struct StatusReaderDescriptor {
    std::uint16_t reader = 0;
    StatusBinding binding;
};

struct StatusEnclosureView {
    bool present = false;
    std::uint16_t id = 0;
    StatusText model;
    StatusIdentity identity;
    StatusLimits limits;
};

struct StatusUsbFunctionView {
    std::uint16_t id = 0;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::uint8_t mode = 0; /* 0 unknown, 1 cold, 2 runtime */
    StatusLocation location;
    StatusText serial;
    StatusText model;
    bool present = false;
    StatusError last_error;
};

struct StatusTuneView {
    std::uint64_t revision = 0;
    std::uint8_t system = 1; /* 1 isdb_t, 2 isdb_s */
    std::uint64_t frequency_hz = 0;
    StatusOptional<std::uint64_t> programmed_hz;
    std::uint8_t selection = 0; /* 0 automatic, 1 tsid, 2 slot */
    std::uint16_t selection_value = 0;
    std::uint8_t verification = 0; /* 0 commands, 1 lock, 2 ts_sync */
};

struct StatusMeasurementView {
    std::uint8_t state = 0; /* 0 unsupported, 1 unknown, 2 valid */
    std::uint8_t reason = 0; /* 1..5 for unknown */
    bool boolean_value = false;
    std::int32_t integer_value = 0;
    std::uint64_t counter_value = 0;
    std::uint64_t sampled_at_ns = 0;
};

struct StatusCounterView {
    std::uint8_t id = 0;
    StatusMeasurementView measurement;
    bool overflow = false;
};

struct StatusMetricView {
    std::uint8_t id = 0;
    StatusMeasurementView measurement;
};

struct StatusCountersView {
    StatusArray<StatusCounterView> entries;
};

struct StatusMeasurementsView {
    StatusArray<StatusMetricView> entries;
};

struct StatusStreamView {
    std::uint64_t id = 0;
    std::uint64_t tune_revision = 0;
    std::uint8_t phase = 1; /* 1 armed, 2 attaching, 3 active, 4 closing */
};

struct StatusStreamFinalView {
    std::uint64_t id = 0;
    std::uint64_t tune_revision = 0;
    std::uint8_t reason = 0; /* 0 stopped .. 7 shutdown */
    StatusError error;
    StatusCountersView statistics;
};

struct StatusReceiverView {
    StatusReceiverDescriptor descriptor;
    bool present = false;
    std::uint8_t state = 0; /* 0 free, 1 leased, 2 tuned, 3 streaming, 4 error, 5 absent */
    std::uint8_t operation = 0; /* 0 idle, 1 tuning, 2 stopping, 3 releasing */
    bool leased = false;
    std::uint64_t tune_revision = 0;
    StatusOptional<StatusTuneView> current_tune;
    StatusOptional<StatusTuneView> last_successful_tune;
    StatusOptional<StatusStreamView> stream;
    StatusOptional<StatusStreamFinalView> last_stream;
    StatusMeasurementsView measurements;
    StatusCountersView receiver_statistics;
    StatusOptional<StatusCountersView> stream_statistics;
    StatusError last_error;
};

struct StatusCardObservation {
    std::uint8_t state = 0; /* 0 unknown, 1 absent, 2 present, 3 unavailable */
    StatusOptional<std::uint64_t> sampled_at_ns;
    std::uint8_t reason = 0;
};

struct StatusReaderView {
    StatusReaderDescriptor descriptor;
    bool present = false;
    StatusCardObservation card;
    std::uint8_t operation = 0; /* 0 idle, 1 initializing, 2 transmitting, 3 resetting, 4 stopping */
    std::uint16_t handles_open = 0;
    bool transaction_active = false;
    StatusError last_error;
};

struct StatusContextView {
    std::array<std::uint8_t, 16> epoch{};
    StatusText instance;
    std::uint8_t state = 0; /* 0 starting, 1 ready, 2 stopping, 3 error, 4 disconnected */
    StatusError last_error;
};

struct StatusSnapshotView {
    std::uint64_t sequence = 0;
    std::uint64_t observed_at_ns = 0;
};

struct StatusScopeView {
    std::uint8_t kind = 0; /* 0 enclosure, 1 receiver, 2 reader, 3 stream */
    std::uint64_t id = 0;
};

struct StatusResponseView {
    StatusContextView context;
    StatusSnapshotView snapshot;
    StatusScopeView scope;
    StatusEnclosureView enclosure;
    StatusOptional<bool> lnb_15v_enabled;
    StatusOptional<bool> card_inserted;
    StatusTopology topology;
    StatusArray<StatusUsbFunctionView> functions;
    StatusArray<StatusReceiverView> receivers;
    StatusArray<StatusReaderView> readers;
};

struct StatusListEnclosureView {
    bool present = false;
    std::uint16_t id = 0;
    StatusLimits limits;
};

struct StatusListResponseView {
    std::array<std::uint8_t, 16> epoch{};
    StatusTopology topology;
    StatusListEnclosureView enclosure;
    StatusArray<StatusUsbFunctionView> functions;
    StatusArray<StatusReceiverDescriptor> receivers;
    StatusArray<StatusReaderDescriptor> readers;
};

struct StatusJsonInput {
    StatusProducerView producer;
    StatusResponseView status;
    StatusListResponseView canonical_list;
};

/* 検証 → 改行込み size 計測 → 容量確認 → 書込み。失敗時は出力先を変更しない。 */
StatusJsonCode status_json_size(const StatusJsonInput *input, std::uint64_t *bytes) noexcept;
StatusJsonCode encode_status_json(const StatusJsonInput *input, dtv_buffer *output) noexcept;

}

#endif
