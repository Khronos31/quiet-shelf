/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TORNE_CECH_TUNE_IO_HPP
#define TORNE_CECH_TUNE_IO_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <dtv/spi/types.h>

#include "cech_tune_signal.hpp"

namespace torne::cech_zd1j {

struct TunePlan;
struct CnrObservation;

struct DemodSyncIoState {
    DemodSyncState machine{};
    bool started = false;
};

struct DemodSyncIoResult {
    DemodSyncStatus status = DemodSyncStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint32_t poll_interval_ms = 0;
    std::size_t plan_steps_completed = 0;
    bool immediate_plan_applied = false;
    bool recovery_attempted = false;
    bool recovery_succeeded = false;
};

enum class WbdCalibrationIoStatus { pending, complete, failed };

enum class WbdCalibrationIoPhase { idle, waiting_measurement, complete, failed };

struct WbdCalibrationIoState {
    TunerCalibrationPlan plan{};
    WbdCalibrationResult calibration{};
    dtv_internal_result failure = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint64_t measurement_ready_ns = 0;
    std::uint16_t previous_offset = 0;
    std::size_t plan_steps_completed = 0;
    WbdCalibrationIoPhase phase = WbdCalibrationIoPhase::idle;
};

struct WbdCalibrationIoResult {
    WbdCalibrationIoStatus status = WbdCalibrationIoStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint32_t poll_interval_ms = 0;
    std::size_t plan_steps_completed = 0;
    bool calibration_valid = false;
    WbdCalibrationResult calibration{};
};

enum class DcCalibrationIoStatus { pending, complete, failed };

enum class DcCalibrationIoPhase { idle, waiting, complete, failed };

struct DcCalibrationIoState {
    DcCalibrationState machine{};
    dtv_internal_result failure = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint64_t next_stage_ns = 0;
    std::size_t plan_steps_completed = 0;
    std::uint8_t calibration_flags_to_clear = 0;
    DcCalibrationIoPhase phase = DcCalibrationIoPhase::idle;
};

struct DcCalibrationIoResult {
    DcCalibrationIoStatus status = DcCalibrationIoStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint32_t poll_interval_ms = 0;
    std::size_t plan_steps_completed = 0;
    std::uint8_t calibration_flags_to_clear = 0;
};

enum class TemperatureCalibrationIoStatus { pending, complete, failed };

enum class TemperatureCalibrationIoPhase {
    idle,
    waiting_first_measurement,
    waiting_second_measurement,
    complete,
    failed
};

struct TemperatureCalibrationIoState {
    TemperatureCalibrationState machine{};
    TunerCalibrationPlan plan{};
    dtv_internal_result failure = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint64_t measurement_ready_ns = 0;
    std::size_t plan_steps_completed = 0;
    std::int16_t temperature = 0;
    bool temperature_valid = false;
    std::uint8_t calibration_flags_to_clear = 0;
    TemperatureCalibrationIoPhase phase = TemperatureCalibrationIoPhase::idle;
};

struct TemperatureCalibrationIoResult {
    TemperatureCalibrationIoStatus status = TemperatureCalibrationIoStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint32_t poll_interval_ms = 0;
    std::size_t plan_steps_completed = 0;
    bool temperature_valid = false;
    std::int16_t temperature = 0;
    std::uint8_t calibration_flags_to_clear = 0;
};

enum class InitialCalibrationPassStatus { pending, complete, failed };

enum class InitialCalibrationPassPhase { idle, running, complete, failed };

enum class InitialCalibrationPassStage { none, dc, wbd, temperature };

struct InitialCalibrationPassInput {
    std::uint8_t pending_flags = 0;
    TunerVariant variant = TunerVariant::standard;
    std::uint64_t requested_frequency_hz = 0;
    std::uint64_t retained_rf_frequency_hz = 0;
    std::uint16_t previous_wbd_gain = 0;
    bool previous_wbd_gain_valid = false;
    std::uint16_t previous_wbd_offset = 0;
    bool previous_wbd_offset_valid = false;
    std::int16_t previous_temperature = 0;
    bool previous_temperature_valid = false;
};

struct InitialCalibrationPassState {
    InitialCalibrationPassPhase phase = InitialCalibrationPassPhase::idle;
    InitialCalibrationPassStage active_stage = InitialCalibrationPassStage::none;
    dtv_internal_result failure = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint8_t pending_flags = 0;
    std::uint8_t completed_flags = 0;
    std::uint64_t requested_frequency_hz = 0;
    std::uint64_t retained_rf_frequency_hz = 0;
    TunerVariant variant = TunerVariant::standard;
    DcCalibrationIoState dc{};
    WbdCalibrationIoState wbd{};
    TemperatureCalibrationIoState temperature_io{};
    std::uint16_t wbd_gain = 0;
    bool wbd_gain_valid = false;
    std::uint16_t wbd_offset = 0;
    bool wbd_offset_valid = false;
    std::int16_t temperature = 0;
    bool temperature_valid = false;
};

struct InitialCalibrationPassResult {
    InitialCalibrationPassStatus status = InitialCalibrationPassStatus::failed;
    InitialCalibrationPassStage active_stage = InitialCalibrationPassStage::none;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint32_t poll_interval_ms = 0;
    std::size_t dc_plan_steps_completed = 0;
    std::size_t wbd_plan_steps_completed = 0;
    std::size_t temperature_plan_steps_completed = 0;
    std::uint8_t pending_flags = 0;
    std::uint8_t completed_flags = 0;
    std::uint64_t retained_rf_frequency_hz = 0;
    std::uint16_t wbd_gain = 0;
    bool wbd_gain_valid = false;
    std::uint16_t wbd_offset = 0;
    bool wbd_offset_valid = false;
    std::int16_t temperature = 0;
    bool temperature_valid = false;
};

enum class NormalCapTrimCalibrationIoStatus { pending, complete, failed };

enum class NormalCapTrimCalibrationIoPhase {
    idle,
    waiting_measurement,
    waiting_retune_candidate,
    waiting_retune_final_write,
    complete,
    failed
};

struct NormalCapTrimCalibrationIoState {
    CapTrimCalibrationState machine{};
    CapTrimCalibrationResult calibration{};
    TunerCalibrationPlan plan{};
    dtv_internal_result failure = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint64_t measurement_ready_ns = 0;
    std::size_t plan_steps_completed = 0;
    bool pll_fraction_zero = false;
    NormalCapTrimCalibrationIoPhase phase = NormalCapTrimCalibrationIoPhase::idle;
};

struct NormalCapTrimCalibrationIoResult {
    NormalCapTrimCalibrationIoStatus status = NormalCapTrimCalibrationIoStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint32_t poll_interval_ms = 0;
    std::size_t plan_steps_completed = 0;
    std::uint16_t register_value = 0;
    bool calibration_valid = false;
    std::uint8_t calibration_flags_to_clear = 0;
};

enum class P1gCapTrimCalibrationIoStatus { pending, complete, failed };

enum class P1gCapTrimCalibrationIoPhase { idle, waiting_hardware_search, complete, failed };

struct P1gCapTrimCalibrationIoState {
    TunerCalibrationPlan plan{};
    dtv_internal_result failure = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint64_t measurement_ready_ns = 0;
    std::size_t plan_steps_completed = 0;
    std::uint16_t captrim_value = 0;
    std::uint8_t calibration_flags_to_clear = 0;
    P1gCapTrimCalibrationIoPhase phase = P1gCapTrimCalibrationIoPhase::idle;
};

struct P1gCapTrimCalibrationIoResult {
    P1gCapTrimCalibrationIoStatus status = P1gCapTrimCalibrationIoStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint32_t poll_interval_ms = 0;
    std::size_t plan_steps_completed = 0;
    bool calibration_valid = false;
    std::uint16_t captrim_value = 0;
    std::uint8_t calibration_flags_to_clear = 0;
};

enum class TunerInitializationIoStatus { complete, failed };

struct TunerInitializationIoResult {
    TunerInitializationIoStatus status = TunerInitializationIoStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::size_t plan_steps_completed = 0;
    std::uint16_t identity_raw = 0;
    TunerIdentityValues identity{};
    bool identity_observed = false;
    bool identity_valid = false;
    bool initialization_valid = false;
    std::uint8_t calibration_flags_to_set = 0;
};

enum class DemodSearchIoStatus {
    pending,
    complete,
    failed,
    guard_plan_applied
};

enum class DemodSearchIoPhase {
    idle,
    fft_plan,
    one_pass_plan,
    guard_plan,
    guard_plan_applied,
    complete,
    failed
};

struct DemodSearchIoState {
    DemodSearchIoPhase phase = DemodSearchIoPhase::idle;
    DemodSearchSelection selection{};
    DemodChannelConfig channel{};
    DemodInitPlan plan{};
    std::size_t plan_step_index = 0;
    std::size_t plan_steps_completed = 0;
    std::size_t fft_read_start_index = 0;
    std::uint16_t fft_correlation_words[6]{};
    std::uint16_t fft_status_register = 0;
    std::uint16_t saved_register = 0;
    std::uint16_t saved_value = 0;
    bool saved_value_valid = false;
    std::uint16_t restart_register = 0;
    std::uint16_t restart_value = 0;
    bool restart_value_valid = false;
};

struct DemodSearchIoResult {
    DemodSearchIoStatus status = DemodSearchIoStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::uint32_t poll_interval_ms = 0;
    std::size_t plan_steps_completed = 0;
    bool immediate_plan_applied = false;
};

enum class DemodSearchResultIoStatus { tmcc_not_established, complete, failed };

struct DemodSearchResultIo {
    DemodSearchResultIoStatus status = DemodSearchResultIoStatus::failed;
    dtv_internal_result io_result = dtv_internal_result_make(DTV_INTERNAL_RESULT_NOT_READY, 0);
    std::size_t read_count = 0;
    std::uint16_t register_568 = 0;
    std::uint16_t register_508 = 0;
    std::uint16_t register_570 = 0;
    std::uint16_t register_505 = 0;
    std::uint16_t register_493[3]{};
    std::uint16_t register_499[3]{};
    std::uint16_t register_481[3]{};
    std::uint16_t register_487[3]{};
    bool raw_complete = false;
    DemodSearchResult decoded{};
    bool decoded_valid = false;
};

/* One device session's I2C ticket state; never stores the callback-scoped io view. */
struct I2cCursor {
    std::uint32_t session = 0;
    std::uint16_t sequence = 0;
    std::uint32_t pending_tag = 0;
    std::size_t pending_slot = 0;
    std::size_t pending_read_length = 0;
    std::uint16_t pending_completion_count = 0;
    /* Keep the cursor and Core1 session alive until this tagged reply is retired. */
    bool pending = false;
};

class CechI2cIo {
public:
    CechI2cIo(const dtv_usb_io *io, dtv_timeout timeout,
              const std::atomic<bool> *cancelled, I2cCursor *cursor);

    dtv_internal_result read_demodulator(std::uint16_t reg, std::uint16_t *value);
    dtv_internal_result write_demodulator(std::uint16_t reg, std::uint16_t value);
    dtv_internal_result read_tuner(std::uint8_t reg, std::uint16_t *value);
    dtv_internal_result write_tuner(std::uint8_t reg, std::uint16_t value);
    dtv_internal_result apply_tune_plan(const TunePlan &plan);
    dtv_internal_result apply_tune_plan_before_captrim(const TunePlan &plan);
    dtv_internal_result apply_tune_plan_after_captrim(const TunePlan &plan);
    WbdCalibrationIoResult begin_wbd_calibration(std::uint64_t frequency_hz,
                                                  std::uint16_t previous_gain,
                                                  std::uint16_t previous_offset,
                                                  WbdCalibrationIoState *state);
    WbdCalibrationIoResult poll_wbd_calibration(WbdCalibrationIoState *state);
    DcCalibrationIoResult begin_dc_calibration_io(TunerVariant variant,
                                                   DcCalibrationIoState *state);
    DcCalibrationIoResult poll_dc_calibration_io(DcCalibrationIoState *state);
    TemperatureCalibrationIoResult begin_temperature_calibration_io(
        TemperatureCalibrationIoState *state);
    TemperatureCalibrationIoResult poll_temperature_calibration_io(
        TemperatureCalibrationIoState *state);
    InitialCalibrationPassResult begin_initial_calibration_pass(
        const InitialCalibrationPassInput &input,
        InitialCalibrationPassState *state);
    InitialCalibrationPassResult poll_initial_calibration_pass(
        InitialCalibrationPassState *state);
    NormalCapTrimCalibrationIoResult begin_normal_captrim_calibration_io(
        bool pll_fraction_zero, NormalCapTrimCalibrationIoState *state);
    NormalCapTrimCalibrationIoResult begin_normal_retune_captrim_calibration_io(
        bool pll_fraction_zero, NormalCapTrimCalibrationIoState *state);
    NormalCapTrimCalibrationIoResult poll_normal_captrim_calibration_io(
        NormalCapTrimCalibrationIoState *state);
    P1gCapTrimCalibrationIoResult begin_p1g_captrim_calibration_io(
        P1gCapTrimCalibrationIoState *state);
    P1gCapTrimCalibrationIoResult poll_p1g_captrim_calibration_io(
        P1gCapTrimCalibrationIoState *state);
    // Run synchronously while the callback-scoped I/O view remains valid.
    TunerInitializationIoResult initialize_tuner();
    dtv_internal_result sleep_frontend();
    dtv_internal_result observe_cnr(CnrObservation *observation);
    DemodSyncIoResult begin_sync(DemodTransmissionMode mode, std::uint8_t guard,
                                 bool differential,
                                 bool wait_mpeg,
                                 const DemodLayerConfig (&layers)[3],
                                 DemodSyncIoState *state);
    DemodSyncIoResult begin_sync_from_search_result(
        const DemodSearchIoState *search_state,
        const DemodSearchResultIo *search_result,
        DemodSyncIoState *state);
    DemodSyncIoResult poll_sync(DemodSyncIoState *state);
    DemodSearchIoResult begin_search(std::uint16_t revision, bool mode_auto,
                                     bool guard_auto,
                                     DemodTransmissionMode requested_mode,
                                     std::uint8_t requested_guard,
                                     const DemodChannelConfig *channel,
                                     DemodSearchIoState *state);
    DemodSearchIoResult poll_search(DemodSearchIoState *state);
    DemodSearchResultIo read_search_result(const DemodSearchIoState *state);

private:
    dtv_internal_result transact(std::uint8_t address, std::uint16_t operation,
                                 std::span<const std::uint8_t> output,
                                 std::span<std::uint8_t> input);
    dtv_internal_result apply_tune_plan_writes(const TunePlan &plan,
                                                std::size_t first,
                                                std::size_t last);
    dtv_internal_result initialize_cursor();
    dtv_internal_result finish_pending(std::span<std::uint8_t> input);
    InitialCalibrationPassResult advance_initial_calibration_pass(
        InitialCalibrationPassState *state);

    const dtv_usb_io *io_;
    dtv_timeout timeout_;
    const std::atomic<bool> *cancelled_;
    I2cCursor *cursor_;
};

}

#endif
