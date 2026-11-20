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
    dtv_internal_result observe_cnr(CnrObservation *observation);
    DemodSyncIoResult begin_sync(DemodTransmissionMode mode, bool differential,
                                 bool wait_mpeg,
                                 const DemodLayerConfig (&layers)[3],
                                 DemodSyncIoState *state);
    DemodSyncIoResult poll_sync(DemodSyncIoState *state);

private:
    dtv_internal_result transact(std::uint8_t address, std::uint16_t operation,
                                 std::span<const std::uint8_t> output,
                                 std::span<std::uint8_t> input);
    dtv_internal_result initialize_cursor();
    dtv_internal_result finish_pending(std::span<std::uint8_t> input);

    const dtv_usb_io *io_;
    dtv_timeout timeout_;
    const std::atomic<bool> *cancelled_;
    I2cCursor *cursor_;
};

}

#endif
