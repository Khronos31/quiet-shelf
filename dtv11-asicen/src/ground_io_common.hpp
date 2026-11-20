/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ASICEN_GROUND_IO_COMMON_HPP
#define ASICEN_GROUND_IO_COMMON_HPP

#include <dtv/spi/device_ops.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace asicen::detail::ground_io {

struct IoContext {
    const dtv_usb_io *io;
    dtv_timeout timeout;
    const std::atomic<bool> *cancelled;
};

IoContext context_from_open(const dtv::spi::PublicOpenIo &, dtv_timeout,
                            const std::atomic<bool> *);
dtv_internal_result transfer(const IoContext &, std::uint8_t, std::uint16_t, std::uint16_t,
                              std::uint8_t *, std::size_t, bool, const char *, const char *);
dtv_internal_result read_demodulator(const IoContext &, std::uint8_t, std::uint8_t,
                                     std::uint8_t, std::uint8_t *, const char *, const char *);
dtv_internal_result write_demodulator(const IoContext &, std::uint8_t, std::uint8_t,
                                      std::uint8_t, const char *, const char *);
dtv_internal_result write_demodulator_bytes(const IoContext &, std::uint8_t, std::uint8_t,
                                            std::span<const std::uint8_t>, bool,
                                            const char *, const char *);
dtv_internal_result write_stage(const IoContext &, std::uint8_t, bool,
                                std::span<const std::uint8_t>, const char *, const char *);
dtv_internal_result read_rf_tuner(const IoContext &, std::uint8_t, std::uint8_t,
                                  std::uint8_t *, const char *, const char *);
dtv_internal_result write_rf_tuner(const IoContext &, std::uint8_t, std::uint8_t,
                                   std::uint8_t, const char *, const char *);
dtv_internal_result read_gpio(const IoContext &, std::uint8_t *, const char *);
dtv_internal_result write_gpio_verified(const IoContext &, std::uint8_t, std::uint8_t,
                                        std::uint8_t, const char *, const char *);
dtv_internal_result write_lnb_gpio_verified(const IoContext &, std::uint8_t,
                                            const char *, const char *);

dtv_internal_result transfer(const dtv::spi::PublicTuneRequest &, const dtv::spi::PublicTuneSession &,
                              std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t *,
                              std::size_t, bool, const char *, const char *);
dtv_internal_result read_demodulator(const dtv::spi::PublicTuneRequest &,
                                     const dtv::spi::PublicTuneSession &, std::uint8_t,
                                     std::uint8_t, std::uint8_t, std::uint8_t *,
                                     const char *, const char *);
dtv_internal_result write_demodulator(const dtv::spi::PublicTuneRequest &,
                                      const dtv::spi::PublicTuneSession &, std::uint8_t,
                                      std::uint8_t, std::uint8_t, const char *, const char *);
dtv_internal_result write_demodulator_bytes(const dtv::spi::PublicTuneRequest &,
                                            const dtv::spi::PublicTuneSession &, std::uint8_t,
                                            std::uint8_t, std::span<const std::uint8_t>, bool,
                                            const char *, const char *);
dtv_internal_result write_stage(const dtv::spi::PublicTuneRequest &,
                                const dtv::spi::PublicTuneSession &, std::uint8_t, bool,
                                std::span<const std::uint8_t>, const char *, const char *);
dtv_internal_result read_rf_tuner(const dtv::spi::PublicTuneRequest &,
                                  const dtv::spi::PublicTuneSession &, std::uint8_t,
                                  std::uint8_t, std::uint8_t *, const char *, const char *);
dtv_internal_result write_rf_tuner(const dtv::spi::PublicTuneRequest &,
                                   const dtv::spi::PublicTuneSession &, std::uint8_t,
                                   std::uint8_t, std::uint8_t, const char *, const char *);
dtv_internal_result write_gpio_verified(const dtv::spi::PublicTuneRequest &,
                                        const dtv::spi::PublicTuneSession &, std::uint8_t,
                                        std::uint8_t, std::uint8_t, const char *, const char *);

}

#endif
