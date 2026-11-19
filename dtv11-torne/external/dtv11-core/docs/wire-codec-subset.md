<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# UDTV 1.1 固定長ペイロードコーデック仕様

本文書は、正本 L2 仕様（`spec/dtv11-l2-spec.md` §1/3/4/7、正本 `93d2948d` 決定 60）および凍結外部 byte 契約（`docs/wire-subset.md`）に基づく、UDTV 1.1 上の固定長 payload codec（PR68 / PR73）の実装利用境界および外部契約を記録したものである。

## 概要と位置づけ

- **積層関係:** PR67 / PR73（20 byte UDTV 1.1 envelope codec および増分 `FrameReceiver`）の上に積層された、固定長 typed payload の直列化・非直列化層である。
- **公開境界:** 公開 L3 API（`include/dtv/dtv.h`）は変更せず、Core 内部専用ヘッダー（`include/dtv/internal/wire-subset.hpp`）および実装（`src/wire-subset.cpp`）に閉じる。
- **特性:** 動的メモリ確保なし、I/O なし、時計なし。呼び出し側提供領域の借用規約（`dtv_bytes`, `dtv_buffer`）をそのまま使用する。

## 3 レコード固定長・リトルエンディアン仕様

### 1. HELLO 要求 (`HelloRequest`)
- **opcode / kind:** `0x0001` (`WireOpcode::hello`) / REQUEST (`flags = 0`)
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
  - `min > max`（`min_major > max_major`、または `min_major == max_major && min_minor > max_minor`）の逆転範囲は `WireFrameCode::invalid_argument`。
  - `min <= (1, 1) <= max` のときのみ 1.1 を選択し `WireFrameCode::ok` を返却（version に `{1, 1}` を設定）。
  - 1.1 を含まない範囲（旧 1.0 のみを含む範囲等）は `WireFrameCode::version_mismatch` を返却。
- **純 HELLO codec と接続契約の層の分離:**
  - **純 HELLO codec 層:** 24 byte の固定長バイト列として任意のバージョン範囲（任意の min/max）を直列化・非直列化可能。`required_features` および `optional_features` の全 64 bit をそのまま透過・保存し、codec 側では意味解釈や未知 feature bit の拒否を行わない（不透明ビット透過保持）。
  - **接続契約層（FrameReceiver / client / selector）:**
    - 実クライアントの要求は `min = max = 1.1`、応答選択は 1.1 である。
    - 旧 1.0 ヘッダー（`magic: "UDTV"`, `major: 1`, `minor: 0`）を持つフレームを受信した場合、外郭ヘッダーデコード段階（`FrameReceiver`）で 20 byte ヘッダーを検証した時点で直ちに `WireFrameCode::version_mismatch` を返却し sticky error 状態となる（`FrameReceiver` 自身は socket/I/O/close を持たない）。payload 解釈に到達する前に接続を終了・切断するのは、正本 L2 §3 および決定 60 に従う呼び出し側（host runtime）の責務である。
    - 任意 range を raw codec が持てることと、接続契約における fallback 可否を混同してはならない（現行の実装では 1.1 のみを選択し、1.0 への fallback は行わない）。
- **旧 client feature 完全一致の事実:**
  - 旧 HELLO codec が未知 bit を保持しても、旧 client の HELLO 応答処理は完全な version / feature 一致（余分 bit があれば `PROTOCOL_ERROR`、要求 bit 不足は `UNSUPPORTED`、版違いは `VERSION_MISMATCH`）を要求する。
  - したがって、未知 bit を素通しするだけで旧 client と相互運用できるという仮説は誤りであり、外郭ヘッダーの版を 1.1 へ更新して旧クライアントと安全に分離する決定 60 の採用根拠に準拠している。

### 2. ACQUIRE 要求 (`AcquireRequest`)
- **opcode / kind:** `0x0020` (`WireOpcode::acquire`) / REQUEST (`flags = 0`)
- **ペイロード長:** 正確に 18 byte（UDTV 1.0 / 1.1 共通配置）。
- **フィールド構成:**
  - `epoch`: `std::uint8_t[16]` (offset 0..15、不透明データとしてそのままコピー)
  - `receiver_id`: `std::uint16_t` (offset 16..17、リトルエンディアン)
- **値域:**
  - `receiver_id` は 0、255 超、65535 を含む `std::uint16_t` 全域を受理。

### 3. ACQUIRE 応答 (`AcquireResponse`)
- **opcode / kind:** `0x0020` (`WireOpcode::acquire`) / RESPONSE (`flags = 1`)
- **ペイロード長:** 正確に 24 byte（UDTV 1.0 / 1.1 共通配置）。
- **フィールド構成:**
  - `epoch`: `std::uint8_t[16]` (offset 0..15、不透明データとしてそのままコピー)
  - `lease_id`: `std::uint64_t` (offset 16..23、リトルエンディアン)
- **非 0 lease_id 制約:**
  - `lease_id == 0` は encode / decode とも `WireFrameCode::invalid_argument` で拒否。

## 入力・出力規約と無変更失敗保証

- **デコード (`decode_*`):**
  - ペイロード長が各レコードの正確なサイズ（24 / 18 / 24 byte）と一致しない場合（不足・余剰）、`WireFrameCode::protocol_error` を返却。
  - 不正 view（null ポインタ等）は `WireFrameCode::invalid_argument` を返却。
- **エンコード (`encode_*`):**
  - 出力先バッファの容量（`dest->capacity`）が不足している場合、書き込みを行わずに `WireFrameCode::buffer_too_small` を返却し、**出力先バッファの内容および `dest->size` を一切変更しない（無変更失敗保証）**。
  - 成功時のみ正確なペイロード長を書き込み、`dest->size` を設定。
  - 構造体のメモリ表現（raw struct）を直接流用せず、リトルエンディアンで 1 byte ずつ安全に読み書きする。

## FrameReceiver との接点および未接続境界

- **FrameReceiver（PR67 / PR73）との接点:**
  - 受信時: 将来の host runtime が `FrameReceiver` から取り出した完成 frame の opcode・payload length を確認後、payload を本 codec の `decode_*` へ渡して型付き構造体へ変換する。旧 1.0 外郭ヘッダーを受信した場合、`FrameReceiver` は 20 byte ヘッダー時点で `WireFrameCode::version_mismatch` を返して sticky error 状態となり、payload を読む前に接続を終了・切断するのは呼び出し側（host runtime）の責務である（`FrameReceiver` 自身は socket/I/O/close を持たない）。
  - 送信時: 本 codec の `encode_*` で payload バッファへ直列化した後、PR67 / PR73 `write_header` により 20 byte UDTV 1.1 envelope ヘッダー（major: 1, minor: 1）を付与して送信フレームを生成する。
- **未実装・未接続・後続境界:**
  - **HELLO 応答:** サーバー側の HELLO 応答 codec は未実装。
  - **接続 gate / 判定:** 初回 HELLO 必須性、要求 feature の適合判定、lease 発行・管理、接続世代判定は上位 host runtime の責務であり、本 codec には含めない。
  - **タイマー:** 5 秒未完了タイムアウト判定は接続を保持する host runtime の責務（本 codec は時計・タイマーを持たない）。
  - **全 A2 未接続:** 本 PR73 は envelope / HELLO 版整合（UDTV 1.1）のみであり、全 A2 の error 意味送受信、epoch 回復、detail 送信、接続 gate、client、daemon 完成とは主張しない。正本 L2 §7 / 決定 59 で error payload 配置は既決、決定 60 でエラーコード 1..22 および版 1.1 も既決であるが、現 main / PR73 に codec や gate は未実装である。opcode ごとの operation 文字列名表のみ未採用。
  - **未実装 payload / トランスポート:** error payload codec、他 typed record、新 feature bit の意味付け、TS_DATA 意味論、JSON、CLI、daemon、IPC transport は現 main / PR73 では未実装・未接続。
  - **L2 未決事項:** STATE_EVENT 追加 feature bit、HELLO 上限比較、TS_DATA 0 packet は未決定・未変更（※ error payload 配置は L2 §7 / 決定 59 で既決であり未決ではない）。
