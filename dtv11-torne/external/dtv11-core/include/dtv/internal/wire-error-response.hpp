/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_WIRE_ERROR_RESPONSE_HPP
#define DTV_INTERNAL_WIRE_ERROR_RESPONSE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include <dtv/internal/wire-envelope.hpp>
#include <dtv/types.h>

namespace dtv::internal {

inline constexpr std::size_t kErrorResponseMinPayloadSize = 35;
inline constexpr std::size_t kErrorResponseMaxPayloadSize = 331;
inline constexpr std::size_t kErrorResponseOperationCapacity = 32;
inline constexpr std::size_t kErrorResponseDetailCapacity = 256;

enum class ErrorTargetKind : std::uint8_t {
    enclosure = 0,
    receiver = 1,
    card_reader = 2,
    stream = 3
};

/*
 * L2 §7 と L3 決定59 の ErrorResponse を、固定最大サイズの所有値として表す。
 * 整数はリトルエンディアンで、構造体のメモリ表現は wire へ流さない。
 */
struct ErrorResponse {
    std::array<std::uint8_t, 16> epoch{};
    std::uint32_t code = 0;
    std::array<std::uint8_t, kErrorResponseOperationCapacity> operation{};
    std::uint16_t operation_length = 0;
    std::array<std::uint8_t, kErrorResponseDetailCapacity> detail{};
    std::uint16_t detail_length = 0;
    ErrorTargetKind target_kind = ErrorTargetKind::enclosure;
    std::uint64_t target_id = 0;
    bool has_retry_after_ms = false;
    std::uint32_t retry_after_ms = 0;
    bool has_required_payload_bytes = false;
    std::uint32_t required_payload_bytes = 0;
};

/* 成功時だけ value を更新する。decode 失敗時は value を変更しない。 */
WireFrameCode decode_error_response(dtv_bytes payload, ErrorResponse *value);
/* 値検証→size 計測→容量確認→書込み。失敗時は出力先を変更しない。 */
WireFrameCode encode_error_response(const ErrorResponse &value, dtv_buffer *output);

}

#endif
