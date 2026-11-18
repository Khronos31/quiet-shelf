<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# UDTV 1.0 固定長ペイロードコーデック仕様

本文書は、L2 仕様（`spec/dtv11-l2-spec.md`）および凍結外部 byte 契約（`docs/wire-subset.md`）に基づく、UDTV 1.0 上の固定長 payload codec（PR68）の実装利用境界および外部契約を記録したものである。

## 概要と位置づけ

- **積層関係:** PR67（`wire-envelope-20261118`、20 byte UDTV 1.0 envelope codec および増分 `FrameReceiver`）の上に積層された、固定長 typed payload の直列化・非直列化層である。
- **公開境界:** 公開 L3 API（`include/dtv/dtv.h`）は変更せず、Core 内部専用ヘッダー（`include/dtv/internal/wire-subset.hpp`）および実装（`src/wire-subset.cpp`）に閉じる。
- **特性:** 動的メモリ確保なし、I/O なし、時計なし。呼び出し側提供領域の借用規約（`dtv_bytes`, `dtv_buffer`）をそのまま使用する。

## 3 レコード固定長・リトルエンディアン仕様

### 1. HELLO 要求 (`DtvWireHelloRequest`)
- **opcode / kind:** `0x0001` (`DTV_WIRE_OP_HELLO`) / REQUEST
- **ペイロード長:** 正確に 24 byte。
- **フィールド構成（すべてリトルエンディアン）:**
  - `min_major`: `std::uint16_t` (offset 0..1)
  - `min_minor`: `std::uint16_t` (offset 2..3)
  - `max_major`: `std::uint16_t` (offset 4..5)
  - `max_minor`: `std::uint16_t` (offset 6..7)
  - `required_features`: `std::uint64_t` (offset 8..15)
  - `optional_features`: `std::uint64_t` (offset 16..23)
- **バージョン範囲判定 (`negotiate_hello_version`):**
  - バージョン比較は `(major, minor)` 順で実施。
  - `min > max`（`min_major > max_major`、または `min_major == max_major && min_minor > max_minor`）の逆転範囲は `DTV_WIRE_RESULT_INVALID_ARGUMENT`。
  - `min <= (1, 0) <= max` のときのみ 1.0 を選択し `DTV_WIRE_RESULT_OK` を返却。
  - 1.0 を含まない範囲は `DTV_WIRE_RESULT_VERSION_MISMATCH` を返却。
- **未知 feature bit:**
  - required / optional とも全 64 bit をそのまま透過し、codec 側では意味や未知 bit の判定を行わない。

### 2. ACQUIRE 要求 (`DtvWireAcquireRequest`)
- **opcode / kind:** `0x0020` (`DTV_WIRE_OP_ACQUIRE`) / REQUEST
- **ペイロード長:** 正確に 18 byte。
- **フィールド構成:**
  - `epoch`: `std::uint8_t[16]` (offset 0..15、不透明データとしてそのままコピー)
  - `receiver_id`: `std::uint16_t` (offset 16..17、リトルエンディアン)
- **値域:**
  - `receiver_id` は 0、255 超、65535 を含む `std::uint16_t` 全域を受理。

### 3. ACQUIRE 応答 (`DtvWireAcquireResponse`)
- **opcode / kind:** `0x0020` (`DTV_WIRE_OP_ACQUIRE`) / RESPONSE
- **ペイロード長:** 正確に 24 byte。
- **フィールド構成:**
  - `epoch`: `std::uint8_t[16]` (offset 0..15、不透明データとしてそのままコピー)
  - `lease_id`: `std::uint64_t` (offset 16..23、リトルエンディアン)
- **非 0 lease_id 制約:**
  - `lease_id == 0` は encode / decode とも `DTV_WIRE_RESULT_INVALID_ARGUMENT` で拒否。

## 入力・出力規約と無変更失敗保証

- **デコード (`decode_*`):**
  - ペイロード長が各レコードの正確なサイズ（24 / 18 / 24 byte）と一致しない場合（不足・余剰）、`DTV_WIRE_RESULT_PROTOCOL_ERROR` を返却。
  - 不正 view（null ポインタ等）は `DTV_WIRE_RESULT_INVALID_ARGUMENT` を返却。
- **エンコード (`encode_*`):**
  - 出力先バッファの容量（`dest->capacity`）が不足している場合、書き込みを行わずに `DTV_WIRE_RESULT_BUFFER_TOO_SMALL` を返却し、**出力先バッファの内容および `dest->size` を一切変更しない（無変更失敗保証）**。
  - 成功時のみ正確なペイロード長を書き込み、`dest->size` を設定。
  - 構造体のメモリ表現（raw struct）を直接流用せず、リトルエンディアンで 1 byte ずつ読み書きする。

## FrameReceiver との接点および未接続境界

- **FrameReceiver（PR67）との接点:**
  - 受信時: 将来の host runtime が `FrameReceiver` から取り出した完成 frame の opcode・payload length を確認後、payload を本 codec の `decode_*` へ渡して型付き構造体へ変換する。
  - 送信時: 本 codec の `encode_*` で payload バッファへ直列化した後、PR67 `encode_frame` により 20 byte UDTV 1.0 envelope ヘッダーを付与して送信フレームを生成する。
- **未実装・未接続・後続境界:**
  - **HELLO 応答:** サーバー側の HELLO 応答 codec は未実装。
  - **接続 gate / 判定:** 初回 HELLO 必須性、要求 feature の適合判定、lease 発行・管理、接続世代判定は上位 host runtime の責務であり、本 codec には含めない。
  - **タイマー:** 5 秒未完了タイムアウト判定は接続を保持する host runtime の責務（本 codec は時計・タイマーを持たない）。
  - **未実装 payload / トランスポート:** error payload、他 typed record、新 feature bit の意味付け、TS_DATA 意味論、JSON、CLI、daemon、IPC transport は未実装・未接続。
  - **L2 未決事項:** error payload 配置、STATE_EVENT 追加 feature bit、HELLO 上限比較、TS_DATA 0 packet は未決定・未変更。
  - **先行条件:** 本 PR68 は PR67 の受入・merge を先行条件とする。
