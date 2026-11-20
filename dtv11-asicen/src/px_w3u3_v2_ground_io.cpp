/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "px_w3u3_v2_ground_io.hpp"

#include "ground_io_common.hpp"

#include <array>
#include <atomic>

namespace asicen::px_w3u3_v2::detail {

namespace ground_io = ::asicen::detail::ground_io;

namespace {

dtv_internal_result make_result(std::int32_t code)
{
    return dtv_internal_result_make(code, 0);
}

bool valid_context(const dtv::spi::PublicTuneRequest &request,
                   const dtv::spi::PublicTuneSession &session,
                   std::uint8_t value_width, const GroundTuneIoState *state)
{
    return state != nullptr && request.system == DTV_SYSTEM_ISDB_T &&
           (request.receiver_index == 1 || request.receiver_index == 3) &&
           (value_width == 1 || value_width == 4) && session.begin_effect != nullptr &&
           session.io != nullptr && session.io->control != nullptr &&
           session.io->clock != nullptr &&
           session.io->clock->monotonic_now_ns != nullptr;
}

dtv_internal_result begin_effect_if_needed(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, GroundTuneIoState *state)
{
    if (state->effect_started) return make_result(DTV_INTERNAL_RESULT_OK);
    if (request.cancelled != nullptr &&
        request.cancelled->load(std::memory_order_acquire))
        return make_result(DTV_INTERNAL_RESULT_CANCELLED);

    std::uint64_t operation_epoch = 0;
    const auto result = session.begin_effect(session.user, &operation_epoch);
    if (result.code == DTV_INTERNAL_RESULT_OK) state->effect_started = true;
    return result;
}

dtv_internal_result read_nmi_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint8_t *value, std::uint8_t value_width, GroundTuneIoState *state)
{
    if (!valid_context(request, session, value_width, state) || value == nullptr)
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);

    const auto started = begin_effect_if_needed(request, session, state);
    if (started.code != DTV_INTERNAL_RESULT_OK) return started;

    const auto address = static_cast<std::uint8_t>(request.receiver_index == 1 ? 0x20 : 0x24);
    const std::array<std::uint8_t, 4> select{
        0xfe,
        0xce,
        static_cast<std::uint8_t>(register_address >> 8),
        static_cast<std::uint8_t>(register_address),
    };
    auto result = ground_io::write_stage(request, session, address, false, select,
        "PX-W3U3 V2 NMI読出し選択応答長が不正です",
        "PX-W3U3 V2 NMI読出し選択statusが不正です");
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    const std::array<std::uint8_t, 2> trigger{0xfe, 0xcf};
    result = ground_io::write_stage(request, session, address, true, trigger,
        "PX-W3U3 V2 NMI読出し開始応答長が不正です",
        "PX-W3U3 V2 NMI読出し開始statusが不正です");
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;

    std::array<std::uint8_t, 5> response{};
    result = ground_io::transfer(request, session, 0x19, address, 0, response.data(),
        static_cast<std::size_t>(value_width) + 1, true,
        "PX-W3U3 V2 NMI読出し応答長が不正です",
        "PX-W3U3 V2 NMI読出しstatusが不正です");
    if (result.code != DTV_INTERNAL_RESULT_OK) return result;
    for (std::uint8_t i = 0; i < value_width; ++i) value[i] = response[i + 1];
    return result;
}

}

dtv_internal_result write_nmi_rf_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint32_t value, std::uint8_t value_width, GroundTuneIoState *state)
{
    if (!valid_context(request, session, value_width, state))
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);

    const auto started = begin_effect_if_needed(request, session, state);
    if (started.code != DTV_INTERNAL_RESULT_OK) return started;

    std::array<std::uint8_t, 8> bytes{
        0xfe,
        0xce,
        static_cast<std::uint8_t>(register_address >> 8),
        static_cast<std::uint8_t>(register_address),
    };
    if (register_address == 0x36) value &= ~UINT32_C(0x80);
    for (std::uint8_t i = 0; i < value_width; ++i)
        bytes[4 + i] = static_cast<std::uint8_t>(value >> (i * 8));

    return ground_io::write_stage(request, session,
        request.receiver_index == 1 ? 0x20 : 0x24, true,
        std::span<const std::uint8_t>(bytes.data(), 4 + value_width),
        "PX-W3U3 V2 NMI書込み応答長が不正です",
        "PX-W3U3 V2 NMI書込みstatusが不正です");
}

dtv_internal_result read_nmi_rf_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint8_t *value, GroundTuneIoState *state)
{
    return read_nmi_register(request, session, register_address, value, 1, state);
}

dtv_internal_result write_nmi_demodulator_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint32_t value, GroundTuneIoState *state)
{
    if (!valid_context(request, session, 4, state))
        return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);

    const auto started = begin_effect_if_needed(request, session, state);
    if (started.code != DTV_INTERNAL_RESULT_OK) return started;

    const std::array<std::uint8_t, 8> bytes{
        0xfe,
        0xce,
        static_cast<std::uint8_t>(register_address >> 8),
        static_cast<std::uint8_t>(register_address),
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 24),
    };
    return ground_io::write_stage(request, session,
        request.receiver_index == 1 ? 0x20 : 0x24, false, bytes,
        "PX-W3U3 V2 NMI demod書込み応答長が不正です",
        "PX-W3U3 V2 NMI demod書込みstatusが不正です");
}

dtv_internal_result read_nmi_demodulator_register(
    const dtv::spi::PublicTuneRequest &request,
    const dtv::spi::PublicTuneSession &session, std::uint16_t register_address,
    std::uint32_t *value, GroundTuneIoState *state)
{
    if (value == nullptr) return make_result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    std::array<std::uint8_t, 4> bytes{};
    const auto result = read_nmi_register(request, session, register_address,
                                           bytes.data(), 4, state);
    if (result.code == DTV_INTERNAL_RESULT_OK)
        *value = static_cast<std::uint32_t>(bytes[0]) |
                 (static_cast<std::uint32_t>(bytes[1]) << 8) |
                 (static_cast<std::uint32_t>(bytes[2]) << 16) |
                 (static_cast<std::uint32_t>(bytes[3]) << 24);
    return result;
}

}
