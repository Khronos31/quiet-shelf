/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ground_io_common.hpp"

#include <algorithm>
#include <array>
#include <atomic>

namespace asicen::detail::ground_io {

namespace {

constexpr std::uint8_t vendor_in = 0xc0;
constexpr std::uint8_t request_i2c_read = 0x02;
constexpr std::uint8_t request_i2c_write = 0x03;
constexpr std::uint8_t request_i2c_write_no_stop = 0x14;
constexpr std::uint8_t request_gpio = 0x08;
constexpr std::uint8_t request_stage_fill = 0x0d;
constexpr std::uint8_t request_stage_send = 0x0e;
constexpr std::uint8_t request_i2c_read_nowait = 0x19;
constexpr std::uint8_t rf_tuner_register = 0xfe;

dtv_internal_result make_result(std::int32_t code, const char *diagnostic = nullptr)
{
    auto result = dtv_internal_result_make(code, 0);
    if (diagnostic != nullptr) {
        std::size_t length = 0;
        while (diagnostic[length] != '\0') ++length;
        dtv_internal_result_set_diagnostic(&result, diagnostic, length);
    }
    return result;
}

dtv_timeout remaining_timeout(dtv_timeout timeout, const dtv_runtime_clock &clock)
{
    const auto deadline = timeout.operation_deadline_ns;
    if (deadline == 0) return timeout;
    const auto now = clock.monotonic_now_ns(clock.user);
    const auto remaining = deadline > now ? deadline - now : 0;
    timeout.timeout_ms = remaining / UINT64_C(1000000) +
                         (remaining % UINT64_C(1000000) != 0 ? 1 : 0);
    return timeout;
}

IoContext context_from_tune(const dtv::spi::PublicTuneRequest &request,
                            const dtv::spi::PublicTuneSession &session)
{
    return {session.io, request.timeout, request.cancelled};
}

dtv_internal_result write_gpio_raw(const IoContext &context, std::uint8_t value,
                                   std::uint8_t mask, std::uint8_t expected,
                                   const char *length_diagnostic,
                                   const char *readback_diagnostic)
{
    std::array<std::uint8_t, 1> response{};
    auto result = transfer(context, request_gpio,
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(mask) << 8) | value), 0,
        response.data(), response.size(), false, length_diagnostic, readback_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = transfer(context, request_gpio, 0, 0, response.data(), response.size(), false,
                      length_diagnostic, readback_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if ((response[0] & mask) != (expected & mask))
        return make_result(DTV_INTERNAL_RESULT_USB_IO, readback_diagnostic);
    return result;
}

}

IoContext context_from_open(const dtv::spi::PublicOpenIo &open_io, dtv_timeout timeout,
                            const std::atomic<bool> *cancelled)
{
    return {open_io.io, timeout, cancelled};
}

dtv_internal_result transfer(const IoContext &context, std::uint8_t usb_request,
                             std::uint16_t value, std::uint16_t index,
                             std::uint8_t *response, std::size_t response_size,
                             bool status_byte, const char *length_diagnostic,
                             const char *status_diagnostic)
{
    if (context.io == nullptr || context.io->control == nullptr ||
        context.io->clock == nullptr || context.io->clock->monotonic_now_ns == nullptr ||
        response == nullptr || response_size == 0)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (context.cancelled != nullptr && context.cancelled->load(std::memory_order_acquire))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);

    dtv_buffer buffer{response, response_size, 0};
    const auto result = context.io->control(
        context.io->user, vendor_in, usb_request, value, index, &buffer,
        remaining_timeout(context.timeout, *context.io->clock));
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    if (buffer.size != response_size)
        return make_result(DTV_INTERNAL_RESULT_USB_IO, length_diagnostic);
    if (status_byte && buffer.data[0] != 0x01)
        return make_result(DTV_INTERNAL_RESULT_USB_IO, status_diagnostic);
    return result;
}

dtv_internal_result transfer(const dtv::spi::PublicTuneRequest &request,
                             const dtv::spi::PublicTuneSession &session,
                             std::uint8_t usb_request, std::uint16_t value,
                             std::uint16_t index, std::uint8_t *response,
                             std::size_t response_size, bool status_byte,
                             const char *length_diagnostic, const char *status_diagnostic)
{
    return transfer(context_from_tune(request, session), usb_request, value, index, response,
                    response_size, status_byte, length_diagnostic, status_diagnostic);
}

dtv_internal_result read_demodulator(const IoContext &context, std::uint8_t address,
                                     std::uint8_t reg, std::uint8_t mode,
                                     std::uint8_t *value, const char *length_diagnostic,
                                     const char *status_diagnostic)
{
    std::array<std::uint8_t, 2> response{};
    const auto result = transfer(context, request_i2c_read,
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(reg) << 8) | address),
        mode, response.data(), response.size(), true, length_diagnostic, status_diagnostic);
    if (result.code == DTV_INTERNAL_RESULT_OK) *value = response[1];
    return result;
}

dtv_internal_result read_demodulator(const dtv::spi::PublicTuneRequest &request,
                                     const dtv::spi::PublicTuneSession &session,
                                     std::uint8_t address, std::uint8_t reg, std::uint8_t mode,
                                     std::uint8_t *value, const char *length_diagnostic,
                                     const char *status_diagnostic)
{
    return read_demodulator(context_from_tune(request, session), address, reg, mode, value,
                            length_diagnostic, status_diagnostic);
}

dtv_internal_result write_demodulator(const IoContext &context, std::uint8_t address,
                                      std::uint8_t reg, std::uint8_t value,
                                      const char *length_diagnostic,
                                      const char *status_diagnostic)
{
    std::array<std::uint8_t, 2> response{};
    return transfer(context, request_i2c_write,
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(reg) << 8) | address),
        value, response.data(), response.size(), true, length_diagnostic, status_diagnostic);
}

dtv_internal_result write_demodulator(const dtv::spi::PublicTuneRequest &request,
                                      const dtv::spi::PublicTuneSession &session,
                                      std::uint8_t address, std::uint8_t reg, std::uint8_t value,
                                      const char *length_diagnostic,
                                      const char *status_diagnostic)
{
    return write_demodulator(context_from_tune(request, session), address, reg, value,
                             length_diagnostic, status_diagnostic);
}

dtv_internal_result write_demodulator_bytes(const IoContext &context, std::uint8_t address,
                                            std::uint8_t reg,
                                            std::span<const std::uint8_t> values, bool no_stop,
                                            const char *length_diagnostic,
                                            const char *status_diagnostic)
{
    if (values.empty() || values.size() > 2)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    std::array<std::uint8_t, 3> response{};
    const auto payload = static_cast<std::uint16_t>(values[0] |
        (values.size() > 1 ? static_cast<std::uint16_t>(values[1]) << 8 : 0));
    return transfer(context, no_stop ? request_i2c_write_no_stop : request_i2c_write,
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(reg) << 8) | address),
        payload, response.data(), values.size() + 1, true, length_diagnostic,
        status_diagnostic);
}

dtv_internal_result write_demodulator_bytes(const dtv::spi::PublicTuneRequest &request,
                                            const dtv::spi::PublicTuneSession &session,
                                            std::uint8_t address, std::uint8_t reg,
                                            std::span<const std::uint8_t> values, bool no_stop,
                                            const char *length_diagnostic,
                                            const char *status_diagnostic)
{
    return write_demodulator_bytes(context_from_tune(request, session), address, reg, values,
                                   no_stop, length_diagnostic, status_diagnostic);
}

dtv_internal_result write_stage(const IoContext &context, std::uint8_t address, bool stop,
                                std::span<const std::uint8_t> bytes,
                                const char *length_diagnostic, const char *status_diagnostic)
{
    if (bytes.empty() || bytes.size() > 0x20)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    std::size_t position = 0;
    while (position < bytes.size()) {
        const auto count = std::min<std::size_t>(3, bytes.size() - position);
        const auto data0 = bytes[position];
        const auto data1 = count > 1 ? bytes[position + 1] : 0;
        const auto data2 = count > 2 ? bytes[position + 2] : 0;
        const auto value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data0) << 8) |
                                                       static_cast<std::uint16_t>(position));
        const auto index = static_cast<std::uint16_t>(data1 |
            (static_cast<std::uint16_t>(data2) << 8));
        std::array<std::uint8_t, 4> response{};
        const auto result = transfer(context, request_stage_fill, value, index,
                                     response.data(), count + 1, true,
                                     length_diagnostic, status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
        position += count;
    }
    std::array<std::uint8_t, 0x21> response{};
    const auto value = static_cast<std::uint16_t>(address |
        (stop ? UINT16_C(0x0100) : 0));
    return transfer(context, request_stage_send, value, 0, response.data(),
                    bytes.size() + 1, true, length_diagnostic, status_diagnostic);
}

dtv_internal_result write_stage(const dtv::spi::PublicTuneRequest &request,
                                const dtv::spi::PublicTuneSession &session,
                                std::uint8_t address, bool stop,
                                std::span<const std::uint8_t> bytes,
                                const char *length_diagnostic, const char *status_diagnostic)
{
    return write_stage(context_from_tune(request, session), address, stop, bytes,
                       length_diagnostic, status_diagnostic);
}

dtv_internal_result read_rf_tuner(const IoContext &context, std::uint8_t demodulator,
                                  std::uint8_t reg, std::uint8_t *value,
                                  const char *length_diagnostic,
                                  const char *status_diagnostic)
{
    auto result = write_demodulator_bytes(context, demodulator, rf_tuner_register,
        std::array<std::uint8_t, 2>{0xc6, reg}, false,
        length_diagnostic, status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    result = write_demodulator_bytes(context, demodulator, rf_tuner_register,
        std::array<std::uint8_t, 1>{0xc7}, true,
        length_diagnostic, status_diagnostic);
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    std::array<std::uint8_t, 2> response{};
    result = transfer(context, request_i2c_read_nowait, demodulator, 0,
                      response.data(), response.size(), true,
                      length_diagnostic, status_diagnostic);
    if (result.code == DTV_INTERNAL_RESULT_OK) *value = response[1];
    return result;
}

dtv_internal_result read_rf_tuner(const dtv::spi::PublicTuneRequest &request,
                                  const dtv::spi::PublicTuneSession &session,
                                  std::uint8_t demodulator, std::uint8_t reg, std::uint8_t *value,
                                  const char *length_diagnostic,
                                  const char *status_diagnostic)
{
    return read_rf_tuner(context_from_tune(request, session), demodulator, reg, value,
                         length_diagnostic, status_diagnostic);
}

dtv_internal_result write_rf_tuner(const IoContext &context, std::uint8_t demodulator,
                                   std::uint8_t reg, std::uint8_t value,
                                   const char *length_diagnostic,
                                   const char *status_diagnostic)
{
    const std::array<std::uint8_t, 4> bytes{rf_tuner_register, 0xc6, reg, value};
    return write_stage(context, demodulator, false, bytes,
                       length_diagnostic, status_diagnostic);
}

dtv_internal_result write_rf_tuner_registers(
    const IoContext &context, std::uint8_t demodulator,
    std::span<const std::uint8_t, 6> values,
    const char *length_diagnostic, const char *status_diagnostic)
{
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto result = write_rf_tuner(context, demodulator,
            static_cast<std::uint8_t>(i + 1), values[i],
            length_diagnostic, status_diagnostic);
        if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    }
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

dtv_internal_result write_rf_tuner(const dtv::spi::PublicTuneRequest &request,
                                   const dtv::spi::PublicTuneSession &session,
                                   std::uint8_t demodulator, std::uint8_t reg, std::uint8_t value,
                                   const char *length_diagnostic,
                                   const char *status_diagnostic)
{
    return write_rf_tuner(context_from_tune(request, session), demodulator, reg, value,
                          length_diagnostic, status_diagnostic);
}

dtv_internal_result read_gpio(const IoContext &context, std::uint8_t *value,
                              const char *length_diagnostic)
{
    if (value == nullptr) return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    std::array<std::uint8_t, 1> response{};
    const auto result = transfer(context, request_gpio, 0, 0, response.data(),
                                 response.size(), false, length_diagnostic, nullptr);
    if (result.code == DTV_INTERNAL_RESULT_OK) *value = response[0];
    return result;
}

dtv_internal_result write_gpio_verified(const IoContext &context, std::uint8_t value,
                                        std::uint8_t mask, std::uint8_t expected,
                                        const char *length_diagnostic,
                                        const char *readback_diagnostic)
{
    mask &= 0xdf;
    if (mask == 0) return make_result(DTV_INTERNAL_RESULT_OK);
    return write_gpio_raw(context, value, mask, expected, length_diagnostic,
                          readback_diagnostic);
}

dtv_internal_result write_lnb_gpio_verified(const IoContext &context, std::uint8_t value,
                                            const char *length_diagnostic,
                                            const char *readback_diagnostic)
{
    return write_gpio_raw(context, value, 0x20, value, length_diagnostic,
                          readback_diagnostic);
}

dtv_internal_result write_gpio_verified(const dtv::spi::PublicTuneRequest &request,
                                        const dtv::spi::PublicTuneSession &session,
                                        std::uint8_t value, std::uint8_t mask,
                                        std::uint8_t expected, const char *length_diagnostic,
                                        const char *readback_diagnostic)
{
    return write_gpio_verified(context_from_tune(request, session), value, mask, expected,
                               length_diagnostic, readback_diagnostic);
}

}
