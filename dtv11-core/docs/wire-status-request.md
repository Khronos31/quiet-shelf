<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# UDTV 1.1 STATUS 要求ペイロードコーデック仕様

本文書は、正本 L2 仕様（`spec/dtv11-l2-spec.md` §5、正本 `e8ec8eb9` 決定 60 / 63）および凍結ワイヤ規約（`docs/wire-records.md`）に基づく、UDTV 1.1 における STATUS 要求（opcode `0x0011` / `kOpcodeStatus`、REQUEST `flags = 0`）の 25 バイト固定長 typed payload 純コーデック（PR79）の実装利用境界および外部契約を記録したものである。

## 概要と位置づけ

- **積層関係:** 20 byte UDTV 1.1 外郭 envelope（PR67 / PR73）の内側に積層される、typed payload の直列化・非直列化層である。
  - 外郭ヘッダーの検証（`magic: "UDTV"`, `major: 1`, `minor: 1`）、`opcode: 0x0011`、`flags: 0`、および `request_id`（非 0）の処理は envelope 層の責務であり、本ペイロードコーデック自身はこれらを扱わない。
- **公開境界:** 公開 L3 API（`include/dtv/dtv.h`）は変更せず、Core 内部専用ヘッダー（`include/dtv/internal/wire-status-request.hpp`）および実装（`src/wire-status-request.cpp`）に閉じる。
- **特性:**
  - 動的メモリ確保なし（heap 割り当てゼロ）。
  - I/O なし、ソケットなし、時計・タイマーなし。
  - 呼び出し側提供領域の借用規約（`dtv_bytes`, `dtv_buffer`）をそのまま使用する。
  - 構造体のメモリ直接配置（raw struct cast）を行わず、リトルエンディアンで 1 バイトずつ安全に読み書きする。

## 1. データ型および実在 4 API 定義

### データ型定義（`dtv::internal`）

ヘッダー: `include/dtv/internal/wire-status-request.hpp`

```cpp
namespace dtv::internal {

inline constexpr std::size_t kStatusRequestPayloadSize = 25;

struct Scope {
    enum class Kind : std::uint8_t {
        enclosure = 0,
        receiver = 1,
        reader = 2,
        stream = 3
    };

    Kind kind = Kind::enclosure;
    std::uint64_t id = 0;
};

struct StatusRequest {
    std::array<std::uint8_t, 16> epoch{};
    Scope scope;
};
```

### 実在 4 API 関数

```cpp
/* stream の id=0 は codec では保持し、利用前の scope 検証で拒否する */
WireFrameCode validate_scope(const Scope &scope);
WireFrameCode status_request_payload_size(const StatusRequest &value, std::size_t *size);
WireFrameCode encode_status_request(const StatusRequest &value, dtv_buffer *output);
WireFrameCode decode_status_request(dtv_bytes payload, StatusRequest *value);
```

## 2. 25 バイト ペイロード構文仕様

STATUS 要求のペイロード長は、常に正確に 25 バイト固定である（`kStatusRequestPayloadSize = 25`）。

### フィールド構成（20 バイト外郭ヘッダー直後をオフセット 0 とする）

| オフセット | フィールド名 | 型 / 長さ | エンディアン | 規約・値域・備考 |
| :--- | :--- | :--- | :--- | :--- |
| `0..15` | `epoch` | `opaque[16]` (16 byte) | - | 接続世代・セッションを識別する不透明な 16 バイトバイナリ。中身を解釈・検証せずそのまま保持（全 0 も透過）。照合や適合判定は上位 gate の責務。 |
| `16` | `scope.kind` | `std::uint8_t` (1 byte) | - | 観測スコープ種別 enum (`0`: enclosure, `1`: receiver, `2`: reader, `3`: stream)。4 以上は不正。 |
| `17..24` | `scope.id` | `std::uint64_t` (8 byte) | リトルエンディアン (LE) | スコープ識別子。下位バイトが offset 17、上位バイトが offset 24 に配置される。 |

## 3. Scope 種別と ID の値域制約（kind / ID 関係）

スコープ種別（`Scope::Kind`）ごとに、許容される識別子（`id`）の値域が厳密に定められている:

| 種別 (`kind`) | 対象 | 許容 `id` 値域 | 備考 |
| :--- | :--- | :--- | :--- |
| `0` (`Kind::enclosure`) | 筐体全体 | `id == 0` のみ | 筐体スコープでは常に 0 固定。非 0 は `invalid_argument`。 |
| `1` (`Kind::receiver`) | 受信機 | `0 <= id <= 65535` | 16 ビット整数（`uint16_t`）範囲。65536 以上は `invalid_argument`。 |
| `2` (`Kind::reader`) | カードリーダー | `0 <= id <= 65535` | 16 ビット整数（`uint16_t`）範囲。65536 以上は `invalid_argument`。 |
| `3` (`Kind::stream`) | ストリーム | `0 <= id <= UINT64_MAX` | 64 ビット整数（`uint64_t`）全域。純コーデックは 0 を含む全域を受理・保持する（次節参照）。 |
| `4` 以上 | 未定義 | - | `invalid_argument` として即座に拒否。 |

## 4. 純コーデックと事前スコープ検証の段階差

本実装では、ワイヤ構文の送受信・中継を担う「純コーデック層」と、メッセージを利用・ディスパッチする直前の「論理スコープ検証層」が明確に段階分離されている。

- **純コーデック層 (`status_request_payload_size` / `encode_status_request` / `decode_status_request`):**
  - `stream` の `id == 0` を含む構造体を合法に往復（encode / decode roundtrip）可能であり、そのまま透過・保持する。
  - ワイヤ表現上の構文正当性（型・オフセット・バイト長・基本値域）のみを判定し、実行時の利用可否を強制しない。
- **事前スコープ検証 (`validate_scope`):**
  - 受信要求を実際に処理・ディスパッチする直前に呼び出し側が実行する集約検証関数。
  - 各種別の基本値域制約に加え、**`stream` かつ `id == 0` の要求を `WireFrameCode::invalid_argument` として明確に拒否する**（アクティブストリームは実運用上 1 以上の非 0 ID が必須）。
  - この段階差により、純粋なメッセージ転送・保存・中継と、実行時の論理スコープ検証が安全に分離されている。

## 5. エラー分類と無変更失敗保証

### エラーコード分類（`WireFrameCode`）

- **`WireFrameCode::protocol_error`:**
  - ペイロード長が正確に 25 バイトでない場合（24 バイト以下、または 26 バイト以上）。
  - 不足バイトまたは余剰バイトが存在する電文は、構文プロトコル違反として即座に拒絶される。
- **`WireFrameCode::invalid_argument`:**
  - `decode_status_request`: 出力構造体ポインタ `value == nullptr`、または無効な入力ビュー（`payload.data == nullptr && payload.size != 0`）。
  - `decode_status_request`: 不正な `kind`（`>= 4`）、または種別固有の ID 範囲外（enclosure かつ `id != 0`、receiver/reader かつ `id > 65535`）。
  - `encode_status_request` / `status_request_payload_size`: 不正な `kind`（`>= 4`）、または種別固有の ID 範囲外。
  - `encode_status_request`: 出力バッファポインタ `output == nullptr`、または無効なバッファポインタ（`output->data == nullptr && output->capacity != 0`）。
  - `status_request_payload_size`: `size == nullptr`。
  - `validate_scope`: 上記種別・ID 範囲外、および `stream` かつ `id == 0`。
- **`WireFrameCode::buffer_too_small`:**
  - `encode_status_request`: 出力バッファの容量（`output->capacity`）が 25 バイト未満の場合。

### 無変更失敗保証（Failure Atomicity / Strong Exception Safety）

コーデック関数がいずれかのエラーコード（非 `ok`）を返却する場合、出力先引数の内容は一切変更されない（部分的な書き換えや中間状態の残留を禁止）:
- `decode_status_request` 失敗時: 出力先 `StatusRequest *value` のメモリ内容は呼び出し前の状態を 100% 保持する。
- `encode_status_request` 失敗時: 出力先 `output->data` のバイト列および `output->size` は一切変更されない。
- `status_request_payload_size` 失敗時: 出力先 `*size` は一切変更されない。

## 6. ゴールデンテストケース（2 件）

### Golden 1: 筐体スコープ（全 0、25 バイト）
全バイトが 0 で構成される筐体要求電文:
- `epoch`: `00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` (16 bytes)
- `scope.kind`: `0` (`Kind::enclosure`)
- `scope.id`: `0` (`00 00 00 00 00 00 00 00`)

```
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

- **挙動:** `status_request_payload_size` は 25 を返却。`encode_status_request` で上記バイト列を出力。`decode_status_request` で `enclosure` かつ `id = 0` としてデコード。`validate_scope` も `ok` を返却する。

### Golden 2: 受信機スコープ（受信機 ID 7、25 バイト）
受信機番号 7 を指定した電文（リトルエンディアン配置の検証）:
- `epoch`: `00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` (16 bytes)
- `scope.kind`: `1` (`Kind::receiver`、offset 16 に `0x01`)
- `scope.id`: `7` (`07 00 00 00 00 00 00 00`、offset 17 に `0x07`, offset 18..24 に `0x00`)

```
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 07 00 00 00 00 00 00 00
```

- **挙動:** `decode_status_request` で `receiver` かつ `id = 7` を取得。再エンコードにより入力と完全に一致する 25 バイト列へ復元。

## 7. 後続ゲートとの接点および未接続境界

### 後続ゲート（host runtime / server gate）の責務
以下の機能・判定は、本純コーデックではなく上位の接続ゲート層が担当する:
- **`epoch` の照合:** 受信した `epoch`（16 バイト不透明値）とサーバーの現在世代（epoch）の照合・現在性判定。
- **対象の実在確認:** 指定されたスコープ対象（指定 ID の筐体・受信機・カードリーダー・アクティブストリーム）がサーバー内に実在するかどうかの照合。
- **`NOT_FOUND` エラー応答生成:** 指定対象が存在しない、または epoch 不一致の場合に、エラーコード `2`（`NOT_FOUND`）を持つ ErrorResponse フレームを生成・返却する処理。
- **相関管理:** クライアント側・サーバー側での `request_id` に基づく要求と応答の紐付け・順序制御。
- **STATUS 応答の処理:** `StatusResponse` ペイロード（Producer, Context 等）の直列化・非直列化（後続 PR で共有型を取り込んで実装）。

### IPC 未完成境界
本 PR79 は、STATUS 要求の 25 バイト純コーデック（pure codec）および単体契約テストの実装であり、以下は含まない:
- ソケット通信、プロセス間通信（IPC）、パイプ、ネットワーク I/O。
- 常駐デーモン（daemon）、クライアントライブラリ、CLI コマンド統合。
- STATUS 応答ペイロードの生成・送信・受信。
- 実機（PX-Q3U4 等）や実環境での動作検証。
本純コーデックの存在をもって「IPC 完成」や「STATUS 機能完成」とは呼ばない。

## 関連ドキュメント

- [wire-status-response.md](wire-status-response.md) - STATUS 応答ペイロードコーデック仕様
- [wire-codec-subset.md](wire-codec-subset.md) - HELLO 要求および ACQUIRE 要求/応答ペイロードコーデック仕様
- [wire-hello-list.md](wire-hello-list.md) - HELLO 応答および LIST 要求ペイロードコーデック仕様
- [wire-error-response.md](wire-error-response.md) - ErrorResponse ペイロードコーデック仕様
- [json-status.md](json-status.md) - userland.status JSON 出力仕様
