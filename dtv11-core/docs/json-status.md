<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# userland.status JSON 出力仕様

関数 `dtv::internal::status_json_size` および `encode_status_json` は、検証済みの `StatusResponse` レコードおよび同一 epoch の正規 `ListResponse` を入力として受け取り、1 つのコンパクトな UTF-8 JSON オブジェクトへとシリアライズ（変換出力）する Core 内部の純エンコーダである。

呼び出し側は、`StatusJsonInput` 構造体を介して、同一 epoch を持つ検証済み `StatusResponseView` と正規 `ListResponseView`、および `StatusProducerView` を提供しなければならない。各入力構造体（`StatusText`、`StatusArray<T>`、`StatusOptional<T>`）は関数呼び出し中のみ有効な借用ビュー（borrowed view）であり、エンコーダ内部での動的メモリ割り当ては一切行わない。入力データ、コンテキスト情報、出力バッファ領域は互いに重複してはならず（no overlap）、関数実行中に内容を変更してはならない。

> [!NOTE]
> 本仕様に対応する Core 内部の全欄純エンコーダおよび同一 epoch バリデータ（`src/status-json.cpp`、`include/dtv/internal/status-json.hpp`）は実装済みである。ただし、公開 L3 層の snapshot getter API、CLI コマンド実行、別プロセス daemon / IPC 結合、スキャン観測からの snapshot 自動構築、および wire プロトコル decode からの変換接続は未実装・後続工程である。

## 公開フィールドの対応関係

出力されるルートオブジェクトのフィールドは `schema`、`producer`、`context`、`snapshot`、`scope`、`enclosure`、`topology` である。ルートオブジェクトの `schema` は `name: "userland.status"`、`major: 1`、`minor: 0` で固定出力される。`usb_functions`、`receivers`、`readers` は、対応するコレクションデータが存在する場合にのみ出力に含まれる（既知 topology で reader が 0 件の場合は `readers: []` と出力され、未観測・非存在時のコレクション省略は凍結規範に従う）。

| 出力 JSON パス | データ型および表現規則 |
| --- | --- |
| `schema.name`, `schema.major`, `schema.minor` | 文字列 `"userland.status"`、数値 1、数値 0 |
| `producer.product`, `producer.version`, `producer.core_version` | 文字列。未定義の core バージョンは `null`（※ 例示のバージョン文字列は製品組み込み時の形式例であり、新リリースの実績を主張するものではない） |
| `context.epoch`, `context.instance`, `context.state`, `context.last_error` | 16 バイトバイナリを表現する 32 桁小文字 16 進数の epoch 文字列、null 許容の instance 文字列、可読状態文字列（`"starting"`、`"ready"`、`"stopping"`、`"error"`、`"disconnected"`）、null 許容のエラーオブジェクト |
| `snapshot.sequence`, `snapshot.observed_at_ns` | 64 ビット整数値をそのまま保持する 10 進文字列 |
| `scope.kind`, `scope.id` | 可読種別文字列（wire 仕様の定義済み語彙 `"enclosure"`、`"receiver"`、`"reader"`、`"stream"`）。enclosure の id は `null`、stream の id は 10 進文字列、receiver の id は JSON 数値、reader の id は JSON 数値（リーダー番号）。独自の新しい kind 文字列は新設しない |
| `enclosure.id`, `enclosure.model`, `enclosure.serial` | JSON 数値、モデル名文字列または `null`、シリアル番号文字列または `null` |
| `enclosure.lnb_15v_enabled`, `enclosure.card_inserted` | JSON 真偽値（boolean）または `null` |
| `enclosure.limits.max_concurrent_streams` | JSON 数値または `null` |
| `topology.state`, `topology.expected_function_count` | 可読状態文字列（`"unknown"`、`"partial"`、`"complete"`）、および JSON 数値または `null` |
| `usb_functions[]` | `id`、`usb`、`mode`（`"unknown"`、`"cold"`、`"runtime"`）、`location.usb_path`、`location.bus`、`location.address`、`serial`、`model`、`present`、`last_error` を保持するオブジェクト配列 |
| `receivers[]` | `descriptor`、`present`、`state`（`"free"`、`"leased"`、`"tuned"`、`"streaming"`、`"error"`、`"absent"`）、`operation`（`"idle"`、`"tuning"`、`"stopping"`、`"releasing"`）、`leased`、`tune_revision`、`current_tune`、`last_successful_tune`、`stream`、`last_stream`、`measurements`、`statistics`、`last_error` を保持するオブジェクト配列 |
| `receivers[].descriptor` | `receiver` 番号、`ISDB-T` や `ISDB-S` などの正式名称を格納する `systems` 配列、null 許容の `binding`。存在する binding は数値の `function_id` と `local_id` を持つ。受信機が出力する TS のスクランブル状態は discovery JSON の `receivers[].ts_scrambling` で定義され、status の descriptor はその定義との整合性を保ち、無関係なフィールドを新設しない |
| `receivers[].current_tune`, `receivers[].last_successful_tune` | 10 進文字列 `revision`、`system`、数値 `frequency_hz`、null 許容の数値 `programmed_hz`、`selection`、`verification` を保持する null 許容の選局オブジェクト。`selection.kind` は `auto`、`tsid`、`slot` のいずれかであり、`auto` の場合 `selection.value` は省略される |
| `receivers[].stream` | 10 進文字列の `id` と `tune_revision`、および `phase`（`"armed"`、`"attaching"`、`"active"`、`"closing"`）を保持する null 許容のアクティブストリームオブジェクト |
| `receivers[].last_stream` | 10 進文字列の `id` と `tune_revision`、`reason`（`"stopped"`、`"data_closed"`、`"owner_closed"`、`"attach_expired"`、`"slow_consumer"`、`"device_lost"`、`"backend_error"`、`"shutdown"`）、null 許容の `error`、`statistics` カウンタマップを保持する null 許容の終端ストリームオブジェクト |
| `receivers[].measurements`, `receivers[].statistics` | サポート対象の測定値オブジェクトおよび名前付きカウンタ。各コレクションは型付き STATUS 値で実際に観測されたエントリのみを反映する |
| `receivers[].statistics.receiver`, `receivers[].statistics.stream` | 受信機カウンタマップおよび null 許容のストリームカウンタマップ。各カウンタオブジェクトは `value`、`sampled_at_ns`、`reason`、`overflow` を保持する |
| `readers[]` | `descriptor`、`present`、`card`、`operation`（`"idle"`、`"initializing"`、`"transmitting"`、`"resetting"`、`"stopping"`）、`handles_open`、`transaction_active`、`last_error` を保持するオブジェクト配列。既知 topology で 0 件の場合は空配列 `[]`。`descriptor` は `reader` 番号（数値）および null 許容の `binding`（数値 `function_id` と `local_id`）を保持する。`card` は各 reader で必須のオブジェクト（card オブジェクト自体は null にしない）であり、`state`（`"unknown"`、`"absent"`、`"present"`、`"unavailable"`）、`sampled_at_ns`、null 許容の `reason`（`"not_sampled"`、`"stale"`、`"tune_changed"`、`"device_lost"`、`"read_failed"` 等）を含む。未観測項目は内部の `null` または `reason` で表現する |
| エラーオブジェクト（決定 61 採用仕様） | 既知エラーコード（L3 定義値の非ゼロ 1〜22。0 の OK は有効なエラーレコードとして扱われず拒否される）時は `{code, operation, detail}`。定義外の未知エラー時は `{code: "UNKNOWN", numeric_code, operation, detail}`。※ wire ErrorResponse の `target`、`retry_after_ms`、`required_payload_bytes` は STATUS ErrorRecord には存在せず、STATUS JSON には出力されない |

### カードリーダーと筐体カード観測の区別

- **`readers[]` 配列**: ホストからアクセス可能なスマートカードリーダーの動的状態（オープンハンドル数 `handles_open`、トランザクション状態 `transaction_active`、スマートカード状態 `card.state` など）を要素ごとに表す。既知 topology でリーダーが存在しない場合は空配列 `readers: []` となり、未観測・非存在時のコレクション省略は凍結規範に従う。
- **`readers[].descriptor` の仕様**: 凍結 wire レコードおよび L2 規範に基づき、各要素の `descriptor` はリーダー番号（`reader`）および結合情報 `binding`（数値の `function_id` と `local_id`）を保持する。
- **`enclosure.card_inserted`**: 筐体ハードウェア自身が直接観測・報告した内部カードの挿入フラグ（真偽値または `null`）である。外部リーダー非対応の機器（DT181 等の内部カード処理機種）であっても、筐体内部のカード存在状態として観測される。外部リーダーの状態（`readers[].card.state`）とは明確に区別される。
- **discovery と status の責務の違い**: discovery JSON の `card_reader_supported`（true/false/null）および `card_reader_device`（親デバイス ID/null）は静的な機能対応可否とハードウェア配置を示す。一方、status JSON の `readers[]` はセッションやハンドルを含む実行時の動的オブジェクト配列である。

### wire レコードとの関係

UDTV 1.0 の wire レコード仕様においては、リーダー番号（reader ID）は内部的な識別キーとして保持されており、wire レコードのレイアウト、フィールド順序、feature フラグ、バージョンゲートは一切変更しない。本仕様における `readers[]` 配列およびリーダー番号の保持は、正本 L2 仕様（§1/9）および凍結 `json-status` の仕様決定に厳格に準拠するものであり、独自の単一オブジェクト置換やリーダー番号非公開といった非規範的改変を行わない。また、wire 内の内部 reader key と、discovery の `card_reader_device`（親 USB ファンクション ID）を混同してはならない。

## JSON 出力例

以下に出力例を示す。列挙値の語彙は凍結 wire 仕様および採択済み決定の定義済み識別文字列に従っている。

```json
{
  "schema": {"name": "userland.status", "major": 1, "minor": 0},
  "producer": {"product": "px4", "version": "1.0.0", "core_version": "1.0.0"},
  "context": {
    "epoch": "0123456789abcdef0123456789abcdef",
    "instance": "default",
    "state": "ready",
    "last_error": null
  },
  "snapshot": {
    "sequence": "100",
    "observed_at_ns": "1730900000000000000"
  },
  "scope": {
    "kind": "enclosure",
    "id": null
  },
  "enclosure": {
    "id": 1,
    "model": "PX-Q3U4",
    "serial": "serial-value",
    "lnb_15v_enabled": false,
    "card_inserted": true,
    "limits": {
      "max_concurrent_streams": 4
    }
  },
  "topology": {
    "state": "complete",
    "expected_function_count": 1
  },
  "usb_functions": [{
    "id": 2,
    "usb": "0511:084a",
    "mode": "runtime",
    "location": {
      "usb_path": "1-2.3",
      "bus": 1,
      "address": 6
    },
    "serial": "usb-serial",
    "model": "PX-Q3U4",
    "present": true,
    "last_error": null
  }],
  "receivers": [{
    "descriptor": {
      "receiver": 3,
      "systems": ["ISDB-T", "ISDB-S"],
      "binding": {
        "function_id": 2,
        "local_id": 0
      }
    },
    "present": true,
    "state": "free",
    "operation": "idle",
    "leased": false,
    "tune_revision": "0",
    "current_tune": null,
    "last_successful_tune": null,
    "stream": null,
    "last_stream": null,
    "measurements": {},
    "statistics": {
      "receiver": {},
      "stream": null
    },
    "last_error": null
  }],
  "readers": [{
    "descriptor": {
      "reader": 0,
      "binding": {
        "function_id": 2,
        "local_id": 0
      }
    },
    "present": true,
    "card": {
      "state": "present",
      "sampled_at_ns": "1730900000000000000",
      "reason": null
    },
    "operation": "idle",
    "handles_open": 0,
    "transaction_active": false,
    "last_error": null
  }]
}
```

`context`、`limits`、`measurements`、`statistics`、`tune.selection.kind`、`stream.phase` などのキー名は、採択済みのマッピング仕様をそのまま維持している。ここでの定義は外部公開される JSON の形状を規定するものであり、採択済みの STATUS 仕様決定事項や Core により検証された値の範囲を超えて、追加のフィールドセマンティクスを創出するものではない。

STATUS および discovery JSON における公開 `model` 値はモデル名のみ（name-only）であり、文字列または `null` で表現される。モデル識別用の内部 ID は出力しない（形式例: `"PX-Q3U4"`）。これは採択済みの仕様決定（回答 2.A / RC47 決定）に準拠したものである。`receivers[].descriptor.capabilities` は本 STATUS JSON 出力では出力しない。これは能力情報の公開自体を取りやめるものではなく、静的なリーダーおよび LNB 構成は discovery JSON 側で公開される。LNA の設定値および既定値のみが後続の設計検討のために保留されており、その詳細仕様はここでは規定しない。

`usb_functions[]` は、内部状態フィールドである `serial_state` や `identification_reason` を出力しない。これらのフィールドは Core 内部の照会・識別処理の状態を保持するものであり、公開 JSON 出力の語彙には含まれない。

非存在の optional 文字列または未観測の項目は、JSON の `null` として出力する。内部で使用される `IdentitySerialState`、`Uniqueness`、`UsbFunction::serial_state`、`UsbFunction::identification_reason` などの型定義は、公開 status の語彙ではない。`serial_unique` はスキャン全体の機器検出結果であり、単一の STATUS 応答から算出されるものではない。筐体レベルのカード存在フラグは内部での直接観測値であり、リーダー状態から集約されたものではない（外部リーダーの状態は `readers[].card` 配下に個別に維持される）。また、本 STATUS JSON 出力がスキャン全体の検出階層（`enclosures[].devices[].receivers[]`）を構築することはない。

## エンコーダの利用条件と出力保証

Core 内部層に実装された出力関数（`dtv::internal::status_json_size` および `encode_status_json`）は、以下の利用条件および保証を提供する。

1. **完全な整合性検証:**
   処理開始時に、`StatusResponseView` と同一 epoch の正規 `ListResponseView` に対する完全な整合性検証を実行する（epoch 一致、ID 一意性・所属、descriptor の systems / binding / capabilities 一致、limits、時刻、last_stream / leased、measurement_mask、counter 必須性・昇順性・overflow、scope およびコレクション presence の全検証）。
2. **厳密なサイズ算出:**
   `status_json_size` は、末尾の改行文字（`\n`）を 1 つ含む厳密な出力バイト数を算出し、`bytes` 引数へ格納する。入力検証に失敗した場合は `StatusJsonCode::invalid_argument` を返し、`bytes` 引数の内容を変更しない。
3. **無変更失敗保証（Failure Invariance）:**
   `encode_status_json` は、実際の書き込み前に事前計測を行い、入力不正（`invalid_argument`）または出力先バッファ容量不足（`buffer_too_small`、`output->capacity` が必要バイト数未満）が検出された場合、呼び出し側の出力バッファ内容および `output->size` を一切変更せずにエラーコードを返す。
4. **安全な直列化:**
   文字列のエスケープ処理では、JSON 規格に従い二重引用符、バックスラッシュ、制御文字（`\u0000`〜`\u001f`）をエスケープし、正当な UTF-8 バイト列を出力する。終端 NUL 文字は付与せず、内部での動的メモリ割り当てを行わない。

このサイズ計測付き安全ライターの動作仕様は、修正された公開マッピング仕様のために独自に設計・実装されたものである。以前の Core #13 JSON 実装は、サイズ事前計測、文字エスケープ、書き込み前検証のアプローチに関してのみ参考とし、その古い STATUS フィールドマッピングおよびテストフィクスチャは公開契約としない。本仕様のマッピングは、2026-10-19 に採択された JSON 決定、2026-10-22 の型付き STATUS レイアウト決定、および決定 61（error object 全 key 仕様）に厳格に準拠している。設計の背景および詳細範囲については、先行リポジトリ `dtv-userland-core` の `docs/json-status.md` および Issue 25 / 26 の決定事項を参照のこと。
