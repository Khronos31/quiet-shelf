/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TORNE_CECH_TUNE_SIGNAL_HPP
#define TORNE_CECH_TUNE_SIGNAL_HPP

#include <cstddef>
#include <cstdint>

namespace torne::cech_zd1j {

enum class TunerVariant { standard, p1g };
enum class FrequencyBand { c_band, vhf, uhf };

struct RegisterWrite {
    std::uint8_t address;
    std::uint16_t value;
};

struct TunePlan {
    std::uint32_t frequency_khz;
    std::uint32_t rf_request_khz;
    FrequencyBand band;
    RegisterWrite writes[14];
    std::size_t write_count;
};

bool build_tune_plan(std::uint64_t frequency_hz, std::uint8_t package,
                     std::uint8_t revision, TunePlan *plan);

enum class InitTarget { device_memory, demodulator, tuner };
enum class InitStepKind {
    read,
    write,
    masked_write,
    write_saved, // 直前に読み出した同一レジスタの値を書き戻す
    delay_ms
};

enum class StreamStepKind {
    write24,
    masked_write24,
    trigger_write24,
    wide_write32,
    trigger_wide_write32,
    poll_mask_clear24
};

struct StreamStep {
    StreamStepKind kind;
    std::uint32_t reg;
    std::uint32_t value;
    std::uint32_t mask;
    std::uint32_t timeout_ms;
    std::uint32_t interval_ms;
};

struct StreamPlan {
    StreamStep steps[64];
    std::size_t step_count;
};

bool build_stream_engine_plan(StreamPlan *plan);
bool build_stream_core0_run_plan(StreamPlan *plan);
bool build_stream_input_start_plan(StreamPlan *plan);
bool build_stream_input_close_plan(StreamPlan *plan);

struct InitStep {
    InitTarget target;
    InitStepKind kind;
    std::uint32_t reg;
    std::uint32_t value;
    std::uint32_t mask;
};

struct DemodInitPlan {
    InitStep steps[160];
    std::size_t step_count;
};

bool build_demod_init_plan(DemodInitPlan *plan);

struct DemodAgcTiming {
    std::uint8_t ratio;
    std::uint32_t timf;
};

bool build_demod_agc_profile_plan(bool c_band, std::uint16_t wbd_reference,
                                  DemodInitPlan *plan);
bool calculate_demod_agc_timing(std::uint64_t frequency_hz,
                                DemodAgcTiming *timing);

struct DemodAgcState {
    std::uint8_t stage;
    bool started;
    bool complete;
    bool failed;
};

enum class DemodAgcStatus { in_progress, complete, failed };

bool begin_demod_agc(bool c_band, std::uint16_t wbd_reference,
                     DemodAgcState *state, DemodInitPlan *plan);
DemodAgcStatus advance_demod_agc(DemodAgcState *state,
                                 bool previous_plan_succeeded,
                                 DemodInitPlan *plan);

struct DemodFrequencyPlan {
    DemodAgcTiming timing;
    std::uint32_t dds;
    bool dds_written;
    DemodInitPlan registers;
};

bool build_demod_frequency_plan(std::uint64_t frequency_hz,
                                std::uint32_t tuner_actual_khz,
                                DemodFrequencyPlan *plan);

enum class DemodTransmissionMode { mode_2k, mode_4k, mode_8k };
enum class DemodConstellation { dqpsk, qpsk, qam16, qam64 };
enum class DemodFec { rate_1_2, rate_2_3, rate_3_4, rate_5_6, other };

struct DemodLayerConfig {
    DemodConstellation constellation;
    DemodFec fec;
    std::uint8_t segments;
    std::uint8_t interleave;
};

struct DemodChannelConfig {
    std::uint8_t sequence;
    DemodTransmissionMode mode;
    std::uint8_t guard;
    bool partial_reception;
    bool sb_mode;
    bool searching;
    bool configuration_loaded;
    DemodLayerConfig layers[3];
    std::uint16_t segment_mask;
};

bool calculate_demod_edge_masks(std::uint16_t segment_mask,
                                std::uint16_t *left_edge_mask,
                                std::uint16_t *right_edge_mask);
bool build_demod_channel_plan(const DemodChannelConfig *config,
                              DemodInitPlan *plan);

enum class DemodSearchStrategy { one_pass, two_pass };

struct DemodSearchSelection {
    DemodSearchStrategy strategy;
    DemodTransmissionMode mode;
    std::uint8_t guard;
    std::uint8_t list;
    bool restart_mode;
};

bool select_demod_search(std::uint16_t revision, bool mode_auto, bool guard_auto,
                         DemodTransmissionMode requested_mode,
                         std::uint8_t requested_guard,
                         DemodSearchSelection *selection);
bool build_demod_fft_search_plan(DemodInitPlan *plan);
bool build_demod_one_pass_search_plan(const DemodSearchSelection *selection,
                                      const DemodChannelConfig *channel,
                                      DemodInitPlan *plan);
bool build_demod_guard_search_plan(DemodTransmissionMode mode,
                                   const DemodChannelConfig *search_channel,
                                   DemodInitPlan *plan);
enum class DemodFftSearchResult { pending, mode_2k, mode_4k, mode_8k };
DemodFftSearchResult resolve_demod_fft_search(
    bool completion_transition_observed,
    const std::uint32_t (&correlations)[3]);

struct DemodSearchResult {
    bool inversion;
    DemodTransmissionMode mode;
    std::uint8_t guard;
    bool partial_reception;
    DemodLayerConfig layers[3];
};

void decode_demod_search_result(
    std::uint16_t register_570, std::uint16_t register_505,
    const std::uint16_t (&register_493)[3],
    const std::uint16_t (&register_499)[3],
    const std::uint16_t (&register_481)[3],
    const std::uint16_t (&register_487)[3],
    DemodSearchResult *result);

bool calculate_demod_loop_values(DemodTransmissionMode mode, bool tracking,
                                 std::uint16_t *register_32,
                                 std::uint16_t *register_37);
bool calculate_demod_sync_wait(DemodTransmissionMode mode, std::uint8_t guard,
                               std::uint16_t *sync_wait);
bool select_demod_mpeg_lock_layer(const DemodLayerConfig (&layers)[3],
                                  std::uint8_t *layer_index);
std::uint32_t demod_mpeg_lock_timeout_ms(std::uint8_t layer_a_interleave);
struct LockObservation;
enum class DemodMpegLockStatus { pending, data_lock, unlocked };
DemodMpegLockStatus resolve_demod_mpeg_lock(bool wait_enabled, bool timed_out,
                                             std::uint8_t selected_layer,
                                             const LockObservation *observation);

enum class DemodSyncStage {
    await_coff_cpil,
    await_lmod4,
    viterbi_start,
    await_mpeg,
    failure_recovery,
    complete,
    failed,
    recovery_failed
};

struct DemodSyncState {
    DemodSyncStage stage;
    DemodTransmissionMode mode;
    bool differential;
    bool wait_mpeg;
    DemodLayerConfig layers[3];
    std::uint8_t selected_layer;
    std::uint64_t coff_started_ms;
    std::uint64_t lmod4_deadline_ms;
    std::uint64_t mpeg_deadline_ms;
};

enum class DemodSyncStatus { pending, complete, failed, recovery_failed };

bool begin_demod_sync(DemodTransmissionMode mode, bool differential,
                      bool wait_mpeg,
                      const DemodLayerConfig (&layers)[3],
                      std::uint64_t now_ms, DemodSyncState *state,
                      DemodInitPlan *plan, std::uint32_t *poll_interval_ms);
DemodSyncStatus advance_demod_sync(DemodSyncState *state,
                                   bool read_succeeded,
                                   bool register_188_read_succeeded,
                                   bool previous_plan_succeeded,
                                   std::uint64_t now_ms,
                                   std::uint16_t register_188,
                                   const LockObservation *observation,
                                   DemodInitPlan *plan,
                                   std::uint32_t *poll_interval_ms);

struct FrontendPowerPlan {
    InitStep steps[5];
    std::size_t step_count;
};

bool build_frontend_power_plan(FrontendPowerPlan *plan);

struct TunerInitPlan {
    InitStep steps[80];
    std::size_t step_count;
};

bool build_tuner_init_plan(std::uint16_t identity, std::uint16_t second_identity,
                           std::uint16_t redundancy_a, std::uint16_t redundancy_b,
                           std::uint16_t calibration_measurement, TunerInitPlan *plan);

struct TunerCalibrationPlan {
    InitStep steps[6];
    std::size_t step_count;
    std::uint16_t calibration_gain;
    std::uint16_t low_tracking_gain;
    std::uint16_t wait_tenth_ms;
    std::uint16_t measurement_register;
    std::uint8_t calibration_flags_to_set;
    std::uint8_t calibration_flags_to_clear;
    bool needs_measurement;
    bool tuner_state_stop;
    bool complete;
    bool wbd_calibration;
};

enum TunerCalibrationFlag : std::uint8_t {
    tuner_calibration_dc = 1,
    tuner_calibration_wbd = 2,
    tuner_calibration_temp = 4,
    tuner_calibration_captrim = 8
};

struct WbdCalibrationResult {
    std::uint16_t gain;
    std::uint16_t offset;
    std::uint8_t calibration_flags_to_clear;
};

struct CapTrimCalibrationState {
    std::int16_t candidate;
    std::int16_t best_candidate;
    std::uint16_t step;
    std::uint16_t minimum_difference;
    std::uint8_t updates_remaining;
    bool retune;
    bool started;
    bool complete;
};

struct CapTrimCalibrationResult {
    std::uint16_t register_value;
    std::uint8_t calibration_flags_to_clear;
    std::uint16_t wait_tenth_ms;
    bool needs_measurement;
    bool complete;
};

struct TemperatureCalibrationState {
    std::uint16_t saved_register_13;
    std::uint16_t wbd_mux;
    std::int16_t first_sample;
    bool started;
    bool first_sample_available;
    bool complete;
};

struct DcCalibrationState {
    TunerVariant variant;
    std::uint8_t row;
    std::uint8_t stage;
    std::uint8_t search_step;
    std::uint16_t bb6;
    std::uint16_t bb7;
    std::int32_t minimum_adc_difference;
    std::int32_t positive_adc;
    std::int32_t adc_difference;
    bool failed;
    bool complete;
};

enum class DcCalibrationStatus { in_progress, complete, failed };

bool build_wbd_calibration_plan(std::uint64_t frequency_hz,
                                std::uint16_t previous_gain,
                                TunerCalibrationPlan *plan);
bool complete_wbd_calibration(const TunerCalibrationPlan *plan,
                              bool measurement_available,
                              std::uint16_t measurement,
                              std::uint16_t previous_offset,
                              WbdCalibrationResult *result);
bool dc_calibration_wait(std::uint8_t stage, std::uint16_t *wait_tenth_ms);
std::int8_t dc_initial_direction(std::int32_t difference);
bool dc_sign_matches(std::int32_t difference, std::int32_t minimum_difference);
bool begin_dc_calibration(TunerVariant variant, std::uint16_t register_24,
                          std::uint16_t wbd_mux, std::uint16_t register_23,
                          DcCalibrationState *state,
                          TunerCalibrationPlan *plan);
DcCalibrationStatus advance_dc_calibration(DcCalibrationState *state,
                                           bool measurement_available,
                                           std::uint16_t measurement,
                                           TunerCalibrationPlan *plan);
std::int16_t calculate_tuner_temperature(std::int16_t first,
                                         std::int16_t second);
bool begin_temperature_calibration(std::uint16_t wbd_mux,
                                   std::uint16_t register_13,
                                   TemperatureCalibrationState *state,
                                   TunerCalibrationPlan *plan);
bool continue_temperature_calibration(TemperatureCalibrationState *state,
                                     std::int16_t first_sample,
                                     TunerCalibrationPlan *plan);
bool complete_temperature_calibration(TemperatureCalibrationState *state,
                                      bool second_sample_available,
                                      std::int16_t second_sample,
                                      std::int16_t *temperature,
                                      TunerCalibrationPlan *plan);
std::uint16_t captrim_register_value(std::int16_t candidate);
bool begin_captrim_calibration(bool retune, std::uint16_t current_register,
                               CapTrimCalibrationState *state);
bool build_captrim_start_plan(TunerCalibrationPlan *plan);
bool build_p1g_captrim_calibration_plan(TunerCalibrationPlan *plan);
bool continue_p1g_captrim_calibration(bool previous_io_succeeded,
                                      TunerCalibrationPlan *plan);
bool complete_captrim_measurement(CapTrimCalibrationState *state,
                                  bool measurement_available,
                                  std::uint16_t measurement,
                                  bool pll_fraction_zero,
                                  CapTrimCalibrationResult *result);

bool build_tuner_step1_plan(bool boost, TunerCalibrationPlan *plan);

struct LockObservation {
    bool signal;
    bool carrier;
    bool tmcc_sync;
    bool coff_cpil;
    bool lmod4;
    bool fec_mpeg;
    bool layer_mpeg[3];
    bool layer_viterbi[3];
};

enum class CnrObservationState {
    not_observed,
    invalid_unlocked,
    invalid_noise_zero,
    numeric_unresolved,
    valid
};

struct CnrObservation {
    CnrObservationState state = CnrObservationState::not_observed;
    bool lock_observed = false;
    bool noise_observed = false;
    bool signal_observed = false;
    std::uint16_t lock_register = 0;
    std::uint16_t noise_register = 0;
    std::uint16_t signal_register = 0;
    std::uint64_t noise_q16_16 = 0;
    std::uint64_t signal_q16_16 = 0;
    std::int32_t hundredth_db = 0;
};

bool demod_fec_mpeg_locked(std::uint16_t register_568);
bool calculate_demod_cnr(std::uint16_t noise_register,
                         std::uint16_t signal_register,
                         std::uint64_t *noise_q16_16,
                         std::uint64_t *signal_q16_16,
                         std::int32_t *hundredth_db);

enum class LockReadResult { read_error, unlocked, locked };

LockReadResult observe_lock(bool read_succeeded, std::uint16_t register_568,
                            const std::uint16_t (&viterbi_registers)[3],
                            LockObservation *observation);

}

#endif
