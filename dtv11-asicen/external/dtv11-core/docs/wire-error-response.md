<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# UDTV 1.0 ErrorResponse ペイロードコーデック仕様

本文書は、正本 L2 仕様（`spec/dtv11-l2-spec.md` §7）および正本 L3 決定 59 に基づく、UDTV 1.0 上の ErrorResponse payload codec（A1 純 codec、PR70）の実装利用境界および外部契約を記録したものである。

## 概要と位置づけ

- **積層関係:** PR67（`wire-envelope-20261118`、20 byte UDTV 1.0 envelope codec および増分 `FrameReceiver`）の上に積層され、エラーフレーム（flags=3）のバイト列を直列化・非直列化する Core 内部の純粋なコーデック層である。
- **公開境界:** 公開 L3 API（`include/dtv/dtv.h`）は変更せず、Core 内部専用ヘッダー（`include/dtv/internal/wire-error-response.hpp`）および実装（`src/wire-error-response.cpp`）に閉じる。
- **特性:** 動的メモリ確保なし、I/O なし、時計なし。呼び出し側提供領域の借用規約（`dtv_bytes`, `dtv_buffer`）をそのまま使用する。

## A1 ErrorResponse バイト構文仕様（35〜331 byte）

### 1. メッセージ識別およびエンベロープとの関係
- **エンベロープ flags:** `flags = 3`（要求側 flags 0 に対するエラー応答、または双方向エラー。エンベロープ外郭層 PR67 の責務）。
- **opcode:** 要求時の元 opcode を保持。
- **request_id:** 非 0（クライアント割当の相関識別子を保持）。

### 2. ペイロード構成（可変長 35〜331 byte）
ペイロード長は式 `35 + O + D + 4*R + 4*P` により厳密に決定される（すべての多バイト整数はリトルエンディアン / LE）。
固定最大サイズの所有構造体 `dtv::internal::ErrorResponse` として表現される。

| オフセット | フィールド | 型・サイズ | 規約・値域 |
| :--- | :--- | :--- | :--- |
| `0` | `epoch` | `std::uint8_t[16]` | 16 byte 不透明データ。全 0 も透過し、codec 側で現在性や一致判定は行わない。 |
| `16` | `code` | `std::uint32_t` (LE) | 非 0 必須（0 は拒否）。未知の非 0 コードは数値のまま保持し、L3 名へ対応付けない。 |
| `20` | `operation_length` (`O`) | `std::uint16_t` (LE) | 0〜32 byte (`kErrorResponseOperationCapacity = 32`)。 |
| `22` | `operation` | `std::uint8_t[O]` | NUL バイトを含まない妥当な UTF-8 文字列。 |
| `22 + O` | `detail_length` (`D`) | `std::uint16_t` (LE) | 0〜256 byte (`kErrorResponseDetailCapacity = 256`)。 |
| `24 + O` | `detail` | `std::uint8_t[D]` | NUL バイトを含まない妥当な UTF-8 文字列（空長 0 も decode 構文として受理）。 |
| `24 + O + D` | `target.kind` | `std::uint8_t` | `ErrorTargetKind`: `0` = 筐体 (`enclosure`), `1` = 受信部 (`receiver`), `2` = カードリーダ (`card_reader`), `3` = stream (`stream`)。4 以上は拒否。 |
| `25 + O + D` | `target.id` | `std::uint64_t` (LE) | kind 0（筐体）時は 0 必須。kind 1（受信部）および kind 2（カードリーダ）時は 0〜65535。kind 3 (stream) 時は `u64` 全域。stream target id=0 は純 codec では保存・透過。 |
| `33 + O + D` | `retry_after_ms.tag` (`R`) | `std::uint8_t` | `0` = 不在 (`has_retry_after_ms = false`), `1` = 存在 (`has_retry_after_ms = true`)。2 以上は拒否。 |
| `34 + O + D` | `retry_after_ms.value` | `std::uint32_t` (LE) | tag=1 の場合のみ 4 byte 存在。tag=0 の場合はペイロード上に存在しない。 |
| `34 + O + D + 4*R` | `required_payload_bytes.tag` (`P`) | `std::uint8_t` | `0` = 不在 (`has_required_payload_bytes = false`), `1` = 存在 (`has_required_payload_bytes = true`)。2 以上は拒否。 |
| `35 + O + D + 4*R` | `required_payload_bytes.value` | `std::uint32_t` (LE) | tag=1 の場合のみ 4 byte 存在。tag=0 の場合はペイロード上に存在しない。 |

## 入力・出力規約と無変更失敗保証

- **デコード (`dtv::internal::decode_error_response`):**
  - **`WireFrameCode::protocol_error`:**
    - ペイロード長が最小 35 byte (`kErrorResponseMinPayloadSize`) 未満、最大 331 byte (`kErrorResponseMaxPayloadSize`) 超過、または式 `35 + O + D + 4*R + 4*P` と厳密に一致しない場合（データの不足、または末尾の余剰バイト）。
    - `operation_length > 32`、または `detail_length > 256` の場合。
    - `operation` または `detail` に不正な UTF-8 シーケンスや途中 NUL 文字が含まれる場合。
    - `retry_after_ms.tag > 1`、または `required_payload_bytes.tag > 1` の場合。
  - **`WireFrameCode::invalid_argument`:**
    - `code == 0` の場合。
    - `target.kind > 3` の場合。
    - `target.kind == 0` (筐体) かつ `target.id != 0` の場合。
    - `target.kind` が 1 (受信部) または 2 (カードリーダ) かつ `target.id > 65535` の場合。
    - 不正な入力 view（ポインタが null かつサイズ非 0 等）または出力ポインタ `value` が null の場合。
  - **デコード無変更保証:** デコードが失敗した場合、呼び出し側の受け取り構造体 `*value` の内容は一切変更しない。
- **エンコード (`dtv::internal::encode_error_response`):**
  - **検証順序:** 引数・構造体メンバ検証 → 正確なサイズ計算 → 出力先容量確認 → バッファ書き込み の順で厳密に実行する。
  - **`WireFrameCode::buffer_too_small`:** 出力先バッファの容量（`output->capacity`）が必要バイト数に満たない場合、`WireFrameCode::buffer_too_small` を返却し、**出力先バッファの内容および `output->size` を一切変更しない（無変更失敗保証 / Atomic Output）**。
  - **`WireFrameCode::invalid_argument`:** `code == 0`、kind 不正、id 範囲外、不正 view / null ポインタ等の場合、`WireFrameCode::invalid_argument` を返却。
  - **成功時 (`WireFrameCode::ok`):** 正確なバイト数（35〜331 byte）を出力先に書き込み、`output->size` をその長さに設定する。

## FrameReceiver との接点および上位境界

- **FrameReceiver（PR67）との接点:**
  - `FrameReceiver` が受信したフレームの外郭解析において、flags = 3、request_id 非 0、opcode を確認後、その payload を本 codec の `decode_error_response` へ渡して型付き構造体を取得する。
  - エンベロープ外郭の解析・検証（flags 不正の検出等）は PR67 FrameReceiver の責務であり、本 payload codec の責務と混同しない。
- **未実装・未接続・後続境界:**
  - **番号の L3 意味対応（A2 案）:**
    - エラーコード数値と新 L3 エラー名・意味の対応付け、および意味付き golden は未採用・未実装である。
    - 版 1.1 の A2 案は Issue 25（コメント 6962533022）にてユーザー判断待ちであり、本 A1 純 codec の配置はその判断と独立している。適合済みとは主張しない。
  - **送信側 detail 必須性:** 送信側において detail 文字列を必須とする判定は後段の責務である（本 codec は decode 構文として長さ 0 の空 detail を正常に受理する）。
  - **stream target id = 0 の扱い:** stream target id = 0 は純 codec では保存・透過する。公開・振り分け前の接続 gate が後続で拒否する境界であり、codec 成功を error dispatch 成功とは呼ばない。
  - **接続 gate / ディスパッチャ:** error frame の送受信 gate、request_id 照合、自動再送、daemon/IPC 結合、実機チューナ等は未実装・未接続である。
