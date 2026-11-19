/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_WIRE_ENVELOPE_HPP
#define DTV_INTERNAL_WIRE_ENVELOPE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include <dtv/types.h>

namespace dtv::internal {

/* L2 §3 のエンベロープ判定結果。 */
enum class WireFrameCode {
    ok,
    /* ビューや引数の不正。ヘッダー長の過不足もここに含む。 */
    invalid_argument,
    version_mismatch,
    /* magic、種別の組合せ、payload 上限の違反。 */
    protocol_error,
    /* 呼び出し側の出力領域またはペイロード領域が不足。 */
    buffer_too_small
};

inline constexpr std::size_t kWireHeaderSize = 20;
inline constexpr std::uint32_t kWireMaxControlPayload = 65536;
inline constexpr std::uint32_t kWireMaxTsDataPayload = 1048504;
inline constexpr std::uint16_t kWireOpcodeEventBit = 0x8000;
inline constexpr std::uint16_t kWireOpcodeTsData = 0x8040;

struct WireHeader {
    std::uint16_t opcode = 0;
    std::uint16_t flags = 0;
    std::uint32_t request_id = 0;
    std::uint32_t payload_length = 0;
};

/*
 * 20 byte ヘッダーをリトルエンディアンの整数として読み書きする。
 * 構造体のメモリ表現は wire へ流さない。
 */
WireFrameCode decode_header(dtv_bytes input, WireHeader *header);
WireFrameCode encode_header(const WireHeader &header, dtv_buffer *output);
/* payload.size は header.payload_length と一致しなければならない。 */
WireFrameCode encode_frame(const WireHeader &header, dtv_bytes payload, dtv_buffer *output);

struct WireFrame {
    WireHeader header;
    /* 受信器の呼び出し側領域を借用する。ハンドラから戻るまでだけ有効。 */
    dtv_bytes payload;
};

/*
 * I/O と時計を持たない増分フレーム受信器。呼び出し側の領域に本文を集め、
 * 完成したフレームだけを入力順に一度ずつハンドラへ渡す。
 * エラー後は reset() まで状態を戻さず、再同期もしない。
 */
class FrameReceiver {
public:
    /* ok 以外を返すと受信器は無効化され、その値が status() になる。 */
    using FrameHandler = WireFrameCode (*)(void *user, const WireFrame &frame);

    /* storage は呼び出し側が所有し、この受信器より長く有効でなければならない。 */
    explicit FrameReceiver(dtv_buffer payload_storage);

    WireFrameCode feed(dtv_bytes input, FrameHandler handler, void *user);
    /* 明示 EOF。未完了のフレームが残っていれば PROTOCOL_ERROR。 */
    WireFrameCode finish();
    void reset();

    bool valid() const { return storage_valid_; }
    WireFrameCode status() const { return status_; }
    bool truncated() const { return truncated_; }
    std::size_t pending_size() const { return header_bytes_ + payload_bytes_; }

private:
    WireFrameCode fail(WireFrameCode code);
    WireFrameCode read_header();
    WireFrameCode deliver(FrameHandler handler, void *user);

    dtv_buffer storage_;
    bool storage_valid_;
    WireFrameCode status_ = WireFrameCode::ok;
    bool truncated_ = false;
    std::array<std::uint8_t, kWireHeaderSize> header_data_{};
    std::size_t header_bytes_ = 0;
    WireHeader header_{};
    std::size_t payload_bytes_ = 0;
};

}

#endif
