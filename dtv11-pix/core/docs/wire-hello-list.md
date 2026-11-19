<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# UDTV 1.1 HELLO 応答および LIST 要求ペイロードコーデック仕様

本文書は、正本 L2 仕様（`spec/dtv11-l2-spec.md` §5、正本 `ece223d0` 決定 63）に基づく、UDTV 1.1 における HELLO 応答および LIST 要求の固定・可変長 typed payload 純コーデック（PR74）の実装利用境界および外部契約を記録したものである。

## 概要と位置づけ

- **積層関係:** 20 byte UDTV 1.1 外郭 envelope（PR67 / PR73）の内側に積層される、typed payload の直列化・非直列化層である。
- **公開境界:** 公開 L3 API（`include/dtv/dtv.h`）は変更せず、Core 内部専用ヘッダー（`include/dtv/internal/wire-hello-list.hpp`）および実装（`src/wire-hello-list.cpp`）に閉じる。
- **特性:** 動的メモリ確保はデコード結果の所有文字列（`std::string`）の生成に限定され、I/O なし、時計なし、20 byte envelope なし。

## 1. LIST 要求ペイロード仕様

- **opcode / kind:** `0x0010`（凍結 `kOpcodeList`）/ REQUEST (`flags = 0`) ※本 payload codec 自身は opcode を処理しない
- **ペイロード長:** 正確に 16 byte（`kListRequestSize = 16`）。
- **フィールド構成:**
  - `epoch`: `std::array<std::uint8_t, 16>`（中身を解釈しない不透明な 16 byte）
- **コーデックの動作規則:**
  - 全て 0 の epoch（`00...00`）も純コーデックとしては正常に受理する。
  - 受信 epoch の照合、現在世代との適合判定、および不一致時の `NOT_FOUND`（エラーコード 2）応答の生成は、上位接続層（host runtime / gate）の責務であり、本コーデック内では行わない。
- **デコード (`decode_list_request`):**
  - ペイロード長が正確に 16 byte でない場合（15 byte 以下または 17 byte 以上）、`WireFrameCode::protocol_error` を返却。
  - 出力先構造体への不正ポインタ（null 等）は `WireFrameCode::invalid_argument` を返却。
- **エンコード (`encode_list_request`):**
  - 出力先バッファの容量（`output->capacity`）が 16 byte 未満の場合、`WireFrameCode::buffer_too_small` を返却し、**出力先バッファの内容および `output->size` を一切変更しない（無変更失敗保証）**。
  - 成功時のみ 16 byte を書き込み、`output->size = 16` を設定。
- **対応する LIST 応答ペイロード仕様:** `docs/wire-list-response.md` を参照。

## 2. HELLO 応答ペイロード仕様

- **opcode / kind:** `0x0001`（凍結 `kOpcodeHello`）/ RESPONSE (`flags = 1`) ※本 payload codec 自身は opcode を処理しない
- **ペイロード長:** 可変長。tag 0 の場合は 41〜137 byte、tag 1 の場合は 43〜203 byte（`kHelloResponseMinSize = 41`, `kHelloResponseMaxSize = 203`）。
- **フィールド構成（すべてリトルエンディアン LE、offset は 20 byte 外郭ヘッダー直後を 0 とする）:**

  | offset | フィールド名 | 型 / 長さ | 備考 |
  | --- | --- | --- | --- |
  | 0 | `major` | `std::uint16_t` (LE) | 選択された版の major（実運用接続時は 1） |
  | 2 | `minor` | `std::uint16_t` (LE) | 選択された版の minor（実運用接続時は 1） |
  | 4 | `features` | `std::uint64_t` (LE) | サポート・有効化された機能ビット列 |
  | 12 | `epoch` | 16 byte | 接続セッションを識別する不透明な 16 byte |
  | 28 | `product` 長さ | `std::uint16_t` (LE) | product のバイト長（P: 0〜32） |
  | 30 | `product` | P byte | UTF-8 バイト列、NUL 文字を含まない |
  | 30+P | `version` 長さ | `std::uint16_t` (LE) | version のバイト長（V: 0〜64） |
  | 32+P | `version` | V byte | UTF-8 バイト列、NUL 文字を含まない |
  | 32+P+V | `core_version` tag | `std::uint8_t` | `0`（非存在）または `1`（存在） |
  | 33+P+V | `core_version` 長さ | `std::uint16_t` (LE) | core_version のバイト長（C: 0〜64、tag 1 のみ） |
  | 35+P+V | `core_version` | C byte | UTF-8 バイト列、NUL 文字を含まない（tag 1 のみ） |
  | B | `max_control_payload` | `std::uint32_t` (LE) | 最大制御ペイロード長（B = 33+P+V または 35+P+V+C） |
  | B+4 | `max_ts_bytes` | `std::uint32_t` (LE) | 最大 TS データ長 |

- **ペイロード計算式:**
  - tag 0: `41 + P + V` byte（最小 41 byte、最大 137 byte）
  - tag 1: `43 + P + V + C` byte（最小 43 byte、最大 203 byte）
- **値の所有権（Ownership）:**
  - `HelloResponse` 構造体内の文字列（`product`, `version`, `core_version`）は、**所有された値型（owning values、`std::string` および `std::optional<std::string>`）**である。借用ビュー（borrowed views）ではない。
- **純コーデックの責務と接続層の分離:**
  - **純コーデックの責務:**
    - 未知の feature bit を解釈せずそのまま透過・保持する。
    - 任意の `major`/`minor` 数値をそのまま直列化・非直列化し、特定の版への強制や判定を行わない。
    - 不透明な epoch をそのまま保持する。
  - **上位接続層（host runtime / gate）の責務:**
    - 要求 range `[min, max]` に対する版 1.1 の選択判定、外郭ヘッダーの版との一致確認、相手先上限値の照合、および初回接続 gate 処理。
- **デコード (`decode_hello_response`):**
  - 入力バイト長不足（最小 41 byte 未満）、余剰バイト存在、不正な tag（2 以上）、不正 UTF-8、NUL 文字の埋め込み、文字列上限超過（P > 32、V > 64、C > 64）が検出された場合、`WireFrameCode::protocol_error` を返却。
- **エンコード (`encode_hello_response`, `hello_response_payload_size`):**
  - 不正な入力値（文字列長上限超過、NUL 埋め込み、不正 UTF-8 等）は `WireFrameCode::invalid_argument` を返却。
  - 出力先バッファ容量不足（`output->capacity < size`）は `WireFrameCode::buffer_too_small` を返却し、**出力先バッファの内容および `output->size` を一切変更しない（無変更失敗保証）**。
  - 成功時のみ計算された厳密なペイロード長を書き込み、`output->size` を設定。

## 3. 接点および未接続境界

- **HELLO 要求と応答の区別:**
  - クライアントからの HELLO 要求（PR68 / PR73）は **固定 24 byte**（`HelloRequest`）。
  - サーバーからの HELLO 応答（本 PR74）は **可変長 41〜203 byte**（`HelloResponse`）。
  両者は別個のレコード・構造体であり混同してはならない。
- **未実装・未接続境界:**
  - 本 PR74 は、HELLO 応答および LIST 要求の純ペイロード直列化・非直列化機能（純 codec）のみを Core 内部に追加するものである。
  - 20 byte 外郭 envelope ヘッダー付与、接続 gate / 世代照合、daemon ホストランタイム、クライアント、スキャン統合、CLI、公開 L3 API への接続は含まず、後続工程の責務である。
