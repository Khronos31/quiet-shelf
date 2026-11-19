<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# UDTV 1.1 STATUS 応答ペイロードコーデック仕様

本文書は、正本 L2 仕様（`spec/dtv11-l2-spec.md` §5、正本 `e8ec8eb9` 決定 60 / 63）および凍結ワイヤ規約（`docs/wire-records.md`）に基づく、UDTV 1.1 における STATUS 応答（opcode `0x0011` / `kOpcodeStatus`、RESPONSE `flags = 1`）の型付きペイロード（typed payload）純コーデックおよび文脈バリデータ（PR80）の実装利用境界および外部契約を記録したものである。

## 概要と位置づけ

- **積層関係:** 20 byte UDTV 1.1 外郭 envelope（PR67 / PR73）の内側に積層される、typed payload の直列化・非直列化層である。
  - 外郭ヘッダーの検証（`magic: "UDTV"`, `major: 1`, `minor: 1`）、`opcode: 0x0011`、`flags: 1`、および `request_id`（非 0）の処理は envelope 層の責務であり、本ペイロードコーデック自身はこれらを扱わない。
- **公開境界:** 公開 L3 API（`include/dtv/dtv.h`）は変更せず、Core 内部専用ヘッダー（`include/dtv/internal/wire-status-response.hpp`）および実装（`src/wire-status-response.cpp`）に閉じる。
- **型の再利用と共有:**
  - LIST 応答（PR78 / `include/dtv/internal/wire-list-response.hpp`）の共通型（`ListProducer`, `ListEnclosure`, `ListUsbFunction`, `ListReceiver`, `ListReader`, `ListTopology`, `ListSnapshot`, `ListErrorRecord`, `WireText`, `WireOptional`, `WireArray`）および STATUS 要求（PR79 / `include/dtv/internal/wire-status-request.hpp`）の `Scope` をそのまま再利用し、同一概念の二重定義や検証規則の重複を排除している。
- **特性:**
  - 動的メモリ確保なし（heap 割り当てゼロ、固定容量の所有値型）。
  - I/O なし、ソケットなし、時計・タイマーなし。
  - 呼び出し側提供領域の借用規約（`dtv_bytes`, `dtv_buffer`）をそのまま使用する。
  - 構造体のメモリ直接配置（raw struct cast）を行わず、リトルエンディアンで 1 バイトずつ安全に読み書きする。

## 1. データ型および実在 5 API 定義

### データ型定義（`dtv::internal`）

ヘッダー: `include/dtv/internal/wire-status-response.hpp`

```cpp
namespace dtv::internal {

struct StatusContext {
    std::array<std::uint8_t, 16> epoch{};
    WireOptional<WireText<64>> instance;
    std::uint8_t state = 0;
    WireOptional<ListErrorRecord> last_error;
};

struct StatusTune {
    std::uint64_t revision = 0;
    std::uint8_t system = 0;
    std::uint64_t frequency_hz = 0;
    WireOptional<std::uint64_t> programmed_hz;
    std::uint8_t selection = 0;
    std::uint16_t selection_value = 0;
    std::uint8_t verification = 0;
};

struct StatusStream {
    std::uint64_t id = 0;
    std::uint64_t tune_revision = 0;
    std::uint8_t phase = 0;
};

struct StatusCounterValue {
    std::uint8_t state = 0;
    std::uint8_t reason = 0;
    std::uint64_t value = 0;
    std::uint64_t sampled_at_ns = 0;
    bool overflow = false;
};

struct StatusCounterEntry {
    std::uint8_t id = 0;
    StatusCounterValue counter;
};

struct StatusStreamCounters {
    WireArray<StatusCounterEntry, 16> entries;
};

struct StatusReceiverCounters {
    WireArray<StatusCounterEntry, 4> entries;
};

struct StatusStreamFinal {
    std::uint64_t id = 0;
    std::uint64_t tune_revision = 0;
    std::uint8_t reason = 0;
    WireOptional<ListErrorRecord> error;
    StatusStreamCounters statistics;
};

struct StatusMeasurement {
    std::uint8_t state = 0;
    std::uint8_t reason = 0;
    bool boolean_value = false;
    std::int32_t integer_value = 0;
    std::uint64_t sampled_at_ns = 0;
};

struct StatusMetricEntry {
    std::uint8_t id = 0;
    StatusMeasurement measurement;
};

struct StatusMeasurements {
    WireArray<StatusMetricEntry, 3> entries;
};

struct StatusReceiver {
    ListReceiver descriptor;
    bool present = false;
    std::uint8_t state = 0;
    std::uint8_t operation = 0;
    bool leased = false;
    std::uint64_t tune_revision = 0;
    WireOptional<StatusTune> current_tune;
    WireOptional<StatusTune> last_successful_tune;
    WireOptional<StatusStream> stream;
    WireOptional<StatusStreamFinal> last_stream;
    StatusMeasurements measurements;
    StatusReceiverCounters receiver_statistics;
    WireOptional<StatusStreamCounters> stream_statistics;
    WireOptional<ListErrorRecord> last_error;
};

struct StatusCardObservation {
    std::uint8_t state = 0;
    WireOptional<std::uint64_t> sampled_at_ns;
    std::uint8_t reason = 0;
};

struct StatusReader {
    ListReader descriptor;
    bool present = false;
    StatusCardObservation card;
    std::uint8_t operation = 0;
    std::uint16_t handles_open = 0;
    bool transaction_active = false;
    WireOptional<ListErrorRecord> last_error;
};

struct StatusResponse {
    ListProducer producer;
    StatusContext context;
    ListSnapshot snapshot;
    Scope scope;
    WireOptional<ListEnclosure> enclosure;
    WireOptional<bool> lnb_15v_enabled;
    WireOptional<bool> card_inserted;
    ListTopology topology;
    WireOptional<WireArray<ListUsbFunction, 8>> functions;
    WireOptional<WireArray<StatusReceiver, 32>> receivers;
    WireOptional<WireArray<StatusReader, 8>> readers;
};
```

### 実在 5 API 関数

```cpp
WireFrameCode status_response_payload_size(const StatusResponse &value, std::size_t *size);
WireFrameCode encode_status_response(const StatusResponse &value, dtv_buffer *output);
WireFrameCode decode_status_response(dtv_bytes payload, StatusResponse *value);
WireFrameCode validate_status_response_scope(const StatusResponse &value);
WireFrameCode validate_status_response(const StatusResponse &value, const ListResponse &list);
```

## 2. ペイロード全欄構成と直列化順序

STATUS 応答ペイロードは、以下の順序で厳密に直列化・非直列化される（すべての多バイト整数はリトルエンディアン / LE）:

1. **`producer` (`ListProducer`):**
   - `product`: `u16` byte 長 + strict UTF-8 (0〜32 byte、途中 NUL 禁止)
   - `version`: `u16` byte 長 + strict UTF-8 (0〜64 byte、途中 NUL 禁止)
   - `core_version`: `u8` tag (0=不在, 1=存在) + [存在時のみ: `u16` byte 長 + strict UTF-8 (0〜64 byte)]
2. **`context` (`StatusContext`):**
   - `epoch`: `opaque[16]` (16 バイト不透明値、応答 epoch はここだけに格納され、ペイロード先頭に別の冗長 epoch は付加されない)
   - `instance`: `u8` tag (0/1) + [存在時のみ: `u16` byte 長 + strict UTF-8 (0〜64 byte)]
   - `state`: `u8` enum (`0`: starting, `1`: ready, `2`: stopping, `3`: error, `4`: disconnected)
   - `last_error`: `u8` tag (0/1) + [存在時のみ: `ListErrorRecord` (code `u32` 非 0、operation `u16` 長 + UTF-8 0..32、detail `u16` 長 + UTF-8 0..256)]
3. **`snapshot` (`ListSnapshot`):**
   - `sequence`: `u64` LE
   - `observed_at_ns`: `u64` LE (観測基準時刻ナノ秒)
4. **`scope` (`Scope`):**
   - `kind`: `u8` enum (`0`: enclosure, `1`: receiver, `2`: reader, `3`: stream)
   - `id`: `u64` LE
5. **`enclosure` (`WireOptional<ListEnclosure>`):**
   - `u8` tag (0/1) + [存在時のみ: `ListEnclosure` (id `u16` LE, model, identity, limits)]
6. **`lnb_15v_enabled` (`WireOptional<bool>`):**
   - `u8` tag (0=unknown/unobserved, 1=観測済み) + [tag 1 時のみ: `u8` 0/1 (`false` は tag 1, 0 で表現)]
7. **`card_inserted` (`WireOptional<bool>`):**
   - `u8` tag (0=unknown/unobserved, 1=観測済み) + [tag 1 時のみ: `u8` 0/1 (`false` は tag 1, 0 で表現)]
8. **`topology` (`ListTopology`):**
   - `state`: `u8` enum (`0`: unknown, `1`: partial, `2`: complete)
   - `expected_function_count`: `u8` tag (0/1) + [tag 1 時のみ: `u16` LE]
9. **`functions` (`WireOptional<WireArray<ListUsbFunction, 8>>`):**
   - `u8` tag (0/1) + [tag 1 時のみ: `u16` count (0〜8) + N 件の `ListUsbFunction`]
10. **`receivers` (`WireOptional<WireArray<StatusReceiver, 32>>`):**
    - `u8` tag (0/1) + [tag 1 時のみ: `u16` count (0〜32) + N 件の `StatusReceiver`]
11. **`readers` (`WireOptional<WireArray<StatusReader, 8>>`):**
    - `u8` tag (0/1) + [tag 1 時のみ: `u16` count (0〜8) + N 件の `StatusReader`]

### 子構造体の直列化詳細

- **`StatusReceiver`:**
  - `descriptor` (`ListReceiver`): id `u16` LE, systems `u8` bitmask, binding, capabilities
  - `present`: `u8` (0/1)
  - `state`: `u8` enum (`0`: free, `1`: leased, `2`: tuned, `3`: streaming, `4`: error, `5`: absent)
  - `operation`: `u8` enum (`0`: idle, `1`: tuning, `2`: stopping, `3`: releasing)
  - `leased`: `u8` (0/1)
  - `tune_revision`: `u64` LE
  - `current_tune`: `u8` tag (0/1) + [存在時: `StatusTune`]
  - `last_successful_tune`: `u8` tag (0/1) + [存在時: `StatusTune`]
  - `stream`: `u8` tag (0/1) + [存在時: `StatusStream`]
  - `last_stream`: `u8` tag (0/1) + [存在時: `StatusStreamFinal`]
  - `measurements`: `StatusMeasurements` (count `u16` (0〜3) + N 件の `StatusMetricEntry`: id `u8` (1=lock, 2=cnr_mdb, 3=signal_mdbm), `StatusMeasurement`: state `u8` (1=unknown, 2=valid; 0=unsupported は禁止), reason `u8`, boolean_value `u8`, integer_value `i32` LE, sampled_at_ns `u64` LE)。無条件に固定全欄が並ぶわけではなく、各 receiver の能力マスク（`capabilities.measurement_mask`）に応じて条件付きで直列化される。
  - `receiver_statistics`: `StatusReceiverCounters` (count `u16` (0〜4) + N 件の `StatusCounterEntry`: id `u8`, `StatusCounterValue`: state `u8`, reason `u8`, value `u64` LE, sampled_at_ns `u64` LE, overflow `u8` (0/1))。必須および任意の統計 ID に応じて条件付きで直列化される。
  - `stream_statistics`: `u8` tag (0/1) + [存在時のみ: `StatusStreamCounters`: count `u16` (2〜16、ID 5 と 6 が必須) + N 件の `StatusCounterEntry`]。ストリーム稼働状態に応じて条件付きで直列化される。
  - `last_error`: `u8` tag (0/1) + [存在時: `ListErrorRecord`]
- **`StatusReader`:**
  - `descriptor` (`ListReader`): id `u16` LE, binding
  - `present`: `u8` (0/1)
  - `card` (`StatusCardObservation`): state `u8` (`0`: unknown, `1`: absent, `2`: present, `3`: unavailable), sampled_at_ns tag 0/1 + [tag 1: `u64` LE], reason `u8`
  - `operation`: `u8` enum (`0`: idle, `1`: initializing, `2`: transmitting, `3`: resetting, `4`: stopping)
  - `handles_open`: `u16` LE
  - `transaction_active`: `u8` (0/1)
  - `last_error`: `u8` tag (0/1) + [存在時: `ListErrorRecord`]

## 3. 3 段階の検証階層と責務分離

本実装では、ワイヤ電文の構文妥当性検証から、論理スコープ形状検証、そして正規 LIST 応答との文脈整合性検証まで、3 つの独立した段階が明確に定義されている:

```
[受信バイト列]
      │
      ▼
① decode_status_response (局所構文デコード: bounds, tags, UTF-8, enum, 一意性)
      │
      ├─ 失敗 ──> protocol_error / invalid_argument (出力 StatusResponse は無変更)
      ▼
② validate_status_response_scope (スコープ形状検証: global / receiver / reader / stream)
      │
      ├─ 失敗 ──> invalid_argument
      ▼
③ validate_status_response (正規 LIST 集約検証: 同一 epoch LIST との照合)
      │
      ├─ 照合合格 ──> ok (正規 STATUS 受領完了)
      └─ 不一致 ────> invalid_argument
```

### 第 1 段階: 局所デコード (`decode_status_response`)
- **責務:** 単一のペイロードバイト列から `StatusResponse` 構造体を安全に復元する。
- **検証項目:**
  - ペイロード長境界、tag（0/1）、配列長の上限チェック（functions <= 8, receivers <= 32, readers <= 8, metrics <= 3, counters <= 4/16）。
  - 文字列の strict UTF-8 検証（途中 NUL 禁止）。
  - enum 値域チェック。
  - 同一コレクション内での ID 重複禁止（functions, receivers, readers, metric IDs, counter IDs）。
- **無変更失敗保証（Failure Atomicity）:**
  - デコーダ内部で 2 段階パース（dry-run parse → publish）を採用。電文の途中で構文エラーが検出された場合、出力先 `StatusResponse *value` のメモリ内容は一切変更されない。
- **重要事項:** `decode_status_response` 自体は `ListResponse` を引数に取らない。**局所 decode が通ったことのみをもって正規 STATUS 受領（acceptance）とは呼ばず、後続の別 validator まで通すことが必須である。**

### 第 2 段階: スコープ形状検証 (`validate_status_response_scope`)
- **責務:** 要求された `Scope` 種別（enclosure / receiver / reader / stream）に対して、応答に含まれるコレクション（functions, receivers, readers）の存在/非存在・要素数・ID が合致しているかを判定する。
- **種別ごとの形状契約:**
  - `Scope::Kind::enclosure`:
    - `functions` は常に必須（present）。
    - `topology.state == 0`（unknown）の場合: `receivers` および `readers` は省略（tag 0）でなければならない。
    - `topology.state != 0`（partial / complete）の場合: `receivers` および `readers` は存在（tag 1）でなければならない（0 件は空配列）。
  - `Scope::Kind::receiver`:
    - `functions` および `readers` は省略（tag 0）でなければならない。
    - `receivers` は存在（tag 1）かつ要素数は正確に 1 でなければならず、その `receiver.descriptor.id == scope.id` でなければならない。
  - `Scope::Kind::reader`:
    - `functions` および `receivers` は省略（tag 0）でなければならない。
    - `readers` は存在（tag 1）かつ要素数は正確に 1 でなければならず、その `reader.descriptor.id == scope.id` でなければならない。
  - `Scope::Kind::stream`:
    - `functions` および `readers` は省略（tag 0）でなければならない。
    - `receivers` は存在（tag 1）かつ要素数は正確に 1 でなければならず、当該 receiver 内のアクティブストリーム（`stream.id`）または終端ストリーム（`last_stream.id`）として `scope.id` が一致していなければならない。

### 第 3 段階: 正規 LIST 集約検証 (`validate_status_response`)
- **責務:** 事前に `validate_list_response(list)` に合格している同一 epoch の正規 `ListResponse` を受け取り、動的ステータスと静的構成情報の完全な整合性を検証する。
- **検証項目:**
  - **epoch 一致:** `value.context.epoch == list.epoch`（要求セッションと同一世代の LIST であること）。
  - **観測時刻整合:** すべての測定値（`measurements`）、カウンタ（`receiver_statistics`, `stream_statistics`, `last_stream.statistics`）、およびカード観測（`card.sampled_at_ns`）のサンプリング時刻 `sampled_at_ns` は、`snapshot.observed_at_ns` 以下でなければならない（未来時刻の観測値の禁止）。
  - **Enclosure 一意性制約:** `StatusResponse` の `enclosure` は走査結果（LIST）とは異なり、STATUS 応答の時点で自前の一意性（`uniqueness`）を主張しない制約を満たさなければならない:
    - `serial_state == 0`（unknown）または `1`（present）の場合: `uniqueness == 0`（unknown）必須。
    - `serial_state == 2`（absent）の場合: `uniqueness == 3`（not_applicable）必須。
    - `unique_in_scan` や `duplicate` などの走査一意性判断は LIST / discovery の責務であり、STATUS 応答単体では `uniqueness` は `0` または `3` のみを許す。なお、未確認の LIST / STATUS 間の identity 完全一致を仕様として新たに断言しない。
  - **descriptor および binding 実在照合:**
    - 各 receiver の `descriptor` は LIST 側の正規 `ListReceiver` と完全一致（id, systems, capabilities, binding）しなければならない。
    - 各 reader の `descriptor` は LIST 側の正規 `ListReader` と完全一致（id, binding）しなければならない。
    - 結合先 USB function（`binding.function_id`）が LIST の `functions` 内に実在しなければならない。
  - **測定項目マスク完全一致:**
    - 各 receiver の報告測定項目 ID（`1`=lock, `2`=cnr_mdb, `3`=signal_mdbm）のビットマスクは、LIST 側 descriptor の `capabilities.measurement_mask` と完全に一致しなければならない。
    - 宣言済み測定項目において、`state == 0`（unsupported）のエントリは局所・スコープ・集約の全境界で禁止される。未確定エントリ（`state == 1` / unknown、`reason` 1..5）は aggregate でも適正な測定状態として許可される。
  - **カウンタ制約:**
    - 各カウンタエントリの必須 ID、定義順序、およびオーバーフロー制約（`overflow == true` かつ飽和していない状態の禁止）。
  - **ストリーム制約:**
    - `scope.kind == stream` の場合、対象の受信機が `leased == true` でなければならない（未リース状態のストリームスコープ要求は拒否）。

## 4. エラー分類と無変更失敗保証

### エラーコード分類（`WireFrameCode`）

- **`WireFrameCode::protocol_error`:**
  - デコード時、ペイロードのバイト列が不足している、または余剰バイトが存在する場合。
  - 不正な長さヘッダー、範囲外の配列要素数（functions > 8, receivers > 32, readers > 8, metrics > 3, counters > 4 または stream counters 不正等）。
  - 文字列デコード時、不正なバイト列（strict UTF-8 違反、途中 NUL 文字の混入）。
- **`WireFrameCode::invalid_argument`:**
  - 出力構造体ポインタ `value == nullptr`、または無効な入力ビュー（`payload.data == nullptr && payload.size != 0`）。
  - 出力バッファポインタ `output == nullptr`、または無効なバッファポインタ。
  - サイズ出力ポインタ `size == nullptr`。
  - 定義域外の enum 値、重複した ID（functions, receivers, readers, metric IDs, counter IDs）。
  - 宣言済み measurement における不正な状態（`state == 0` / unsupported）。
  - スコープ形状違反（`validate_status_response_scope`）。
  - 正規 LIST との照合違反（`validate_status_response`）。
  - サイズ算出時、算出サイズが制御上限 `kWireMaxControlPayload`（65,536 バイト）を超過した場合。
- **`WireFrameCode::buffer_too_small`:**
  - エンコード時、出力バッファの容量（`output->capacity`）が算出サイズ未満の場合。

### 無変更失敗保証（Failure Atomicity / Strong Exception Safety）

コーデック関数がいずれかのエラーコード（非 `ok`）を返却する場合、出力先引数の内容は一切変更されない:
- `decode_status_response` 失敗時: 出力先 `StatusResponse *value` のメモリ内容は呼び出し前の状態を 100% 保持する（2 段階パースにより中間状態の書き込みを防止）。
- `encode_status_response` 失敗時: 出力先 `output->data` のバイト列および `output->size` は一切変更されない。
- `status_response_payload_size` 失敗時: 出力先 `*size` は一切変更されない。

## 5. ゴールデン電文とペイロードサイズ

### ゴールデンテストケース（2 件）

- **Golden 1: 59 バイト (global / unknown):**
  - 全省略・最小構成。
  - `scope.kind == Scope::Kind::enclosure`、`topology.state == 0`（unknown）。
  - optional の enclosure, lnb, card は不在（tag 0）。functions は 0 件、receivers および readers は省略（tag 0）。
  - ペイロード長: 正確に 59 バイト。
- **Golden 2: 63 バイト (global / partial-empty):**
  - `scope.kind == Scope::Kind::enclosure`、`topology.state == 1`（partial）。
  - receivers および readers が present かつ既知の 0 件（known empty: tag 1, count 0）。
  - ペイロード長: 正確に 63 バイト。

### 実測受入済み最大候補（58,780 バイト）

単体契約テストにおいて、全項目・全コレクションを上限まで詰め込んだ最大構成のペイロードが実際に組み立てられ、エンコード・デコード・往復一致・集約検証が実測確認されている:
- 構成: 8 USB functions、32 receivers（各 receiver に Tune, Stream, StreamFinal, measurements 3 件, statistics 4 件 / 16 件）、8 readers（各 reader に card, handles, transaction）。
- **実測ペイロードサイズ: 58,780 バイト**。
- **位置づけ:** 本数値（58,780 バイト）は、本コーデック実装において実測受入（constructed and validated）された電文候補のサイズであり、**「数学的な理論最大値」や「全実機での動作保証」を主張するものではない**。
- **制御上限との関係:** UDTV 1.1 の制御フレーム上限 `kWireMaxControlPayload = 65536`（65,536 バイト）に完全に収まる（58,780 <= 65,536）。
- **外郭ヘッダーの除外:** 本サイズ（58,780 バイト）は純粋なペイロードサイズであり、20 バイトの UDTV 1.1 外郭ヘッダー（envelope）は含まない（外郭ヘッダー長と混同しない）。

### wire レコードの Reader と公開 JSON の階層差

- **UDTV 1.1 wire プロトコル:** スマートカードリーダーの動的状態を表す `readers` コレクション（`StatusReader` 配列）が明確に定義され、直列化・非直列化される。
- **L3 公開 JSON (userland.status):** ホスト非公開の内部スマートカードリーダー（内部利用機種等）では `readers` 配列が省略されうる。wire プロトコル上の必須性・定義と、上位 JSON 表現の公開省略規則を混同してはならない。

## 6. 後続ゲートとの接点および未接続境界

### 後続ゲート（host runtime / server gate）の責務
以下の機能・判定は、本純コーデックではなく上位の接続ゲート層が担当する:
- ソケット通信、TCP/UDP、プロセス間通信（IPC）、パイプ、ネットワーク I/O。
- 要求 opcode `0x0011` / flags 0 と応答 flags 1 / `request_id` の紐付け・相関管理。
- 受信した STATUS 要求の `epoch` 照合、対象の実在確認、および対象不在時の `NOT_FOUND`（エラーコード 2）ErrorResponse フレームの生成・返却。
- 走査結果（LIST）の生成・保持、および STATUS 応答構造体への観測値の流し込み（マッピング）。
- 常駐デーモン（daemon）、クライアントライブラリ、CLI コマンド統合。

### 実機および IPC 未完成境界
本 PR80 は、STATUS 応答の型付きペイロード純コーデックおよび文脈バリデータの実装であり、以下は含まない:
- 実機（PX-Q3U4 等）での動的状態観測、ハードウェアレジスタ読み出し、チューナー制御。
- 本純コーデックおよびバリデータの存在をもって「IPC 完成」や「STATUS 機能完成」とは呼ばない。

## 関連ドキュメント

- [wire-status-request.md](wire-status-request.md) - STATUS 要求ペイロードコーデック仕様
- [wire-list-response.md](wire-list-response.md) - LIST 応答ペイロードコーデック仕様
- [wire-hello-list.md](wire-hello-list.md) - HELLO 応答および LIST 要求ペイロードコーデック仕様
- [wire-codec-subset.md](wire-codec-subset.md) - HELLO 要求および ACQUIRE 要求/応答ペイロードコーデック仕様
- [wire-error-response.md](wire-error-response.md) - ErrorResponse ペイロードコーデック仕様
- [json-status.md](json-status.md) - userland.status JSON 出力仕様
