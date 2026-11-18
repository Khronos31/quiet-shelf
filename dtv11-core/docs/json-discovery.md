<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# userland.discovery JSON 出力および CLI 接続仕様

本文書は、正本 L1、L2 §9、L3、および L3 決定 62 / 64 / 65（正本 `209dab21`）に基づく、内部ヘッダー `include/dtv/internal/discovery-json.hpp`（名前空間 `dtv::internal`）および生成 CLI の `--list-json` オプションにおける `userland.discovery` JSON 出力および接続仕様を記録したものである。

## 概要と位置づけ

- **目的:** 単一製品 CLI `--list-json` などにおいて、1 回のハードウェア走査バッチから完全な筐体一覧、孤立 USB デバイス一覧、およびスキャン完了状態を整合して取得し、公開仕様で定義された `userland.discovery` JSON 形式として単一文書を標準出力（stdout）へ出力する。
- **アーキテクチャと境界:**
  - 生成 CLI は起動時に `dtv_load_<product>()` を呼び出して L3 モデル・製品メタデータを登録する。
  - L3 の公開 getter から全 29 欄のデータを収集し、内部の借用型（`dtv::internal::DiscoveryJsonInput`）へ射影した上で、内部エンコーダ（`encode_discovery_json`）により JSON を生成する。
  - 成功時は compact UTF-8 JSON 1 文書と末尾改行（`\n`）1 個を出力する。
  - 内部失敗時（検証失敗・L3 走査失敗・エンコード失敗）は stdout に一切書き出さず（stdout 空）、stderr にエラーを出力して終了する。

## JSON 出力例（実機の物理階層と決定 64 / 65 反映）

出力される JSON は、以下のとおり実機の物理的な階層構造を保持する。位置情報（bus/address/port）は入れ子にせず平らに並べ（L3 決定 64）、容量超過等の問題配列は空配列 `[]` とする（L3 決定 65）。

```json
{
  "schema": {"name": "userland.discovery", "major": 1, "minor": 0},
  "producer": {"product": "px4", "version": "1.0.0"},
  "scan": {"complete": true, "issues": []},
  "enclosures": [{
    "id": 1,
    "model": "PX-Q3U4",
    "serial": null,
    "serial_unique": null,
    "status": "ready",
    "lnb_15v_supported": null,
    "card_reader_supported": true,
    "card_reader_device": 2,
    "devices": [{
      "id": 2,
      "usb": "0511:084a",
      "serial": null,
      "bus": 1,
      "address": 6,
      "port": "1-2.3",
      "receivers": [{
        "receiver": 3,
        "device": 2,
        "local": 0,
        "systems": ["ISDB-T", "ISDB-S"],
        "ts_scrambling": "scrambled"
      }]
    }],
    "candidates": [{
      "device": 0,
      "serial": null,
      "bus": 1,
      "address": 7,
      "port": "1-2.4"
    }]
  }],
  "ungrouped_usb_devices": [{
    "serial": null,
    "model": null,
    "usb": "0511:084a",
    "status": "usable",
    "bus": 1,
    "address": 8,
    "port": "1-2.5"
  }]
}
```

識別子（ID）は JSON の数値として出力する。VID および PID は小文字の 16 進数表記による 4 桁ゼロ詰めの `VID:PID` 文字列とする。受信機構成が未知のデバイスでは `receivers` フィールド自体を省略し、受信機が 0 個であることが確認されているデバイスでは空配列（`[]`）を出力する。

## 走査収集契約と直列化・オブジェクト寿命

1. **L3 登録順序と producer メタデータ:**
   - 生成 CLI `main` が `dtv_load_<product>()` を呼び出し、L3 product / model を登録する。
   - `producer.product` は L3 `dtv_list_models()` の先頭 `model.product` から取得する。筐体一覧が空（0 件）の場合であってもモデル宣言が存在するため確定的に取得可能である。
   - `producer.version` は、CLI がビルド時に焼き込んだ設定値（CMake の `PRODUCT_VERSION`、`dtv_cli_config.version`）を使用する（正本 L2 §9 / L3 決定 62 の明示例外）。
2. **一覧呼び出し順序（一覧 1 回契約）と寿命管理:**
   - CLI `--list-json` の実行時は、`dtv_list_enclosures(&error)` を 1 回だけ呼び出す。NULL かつエラーの場合は即座に終了し、JSON は出力しない。
   - 成功時は `dtv_get_list_capacity()` 個の全枠を走査し、NULL でない各筐体を `dtv_hold_enclosure()` で保持する。
   - 直後に、同一の直列化区間内で各筐体の getter、孤立 USB デバイス（`dtv_list_ungrouped_devices()`）、および完了フラグ（`dtv_get_scan_complete()`）を読み取る。
   - エンコード処理が完了した段階で、保持していた全筐体を `dtv_drop_enclosure()` で解放する。
   - 走査バッチおよび getter 値はその時点の観測値であり、JSON 内に epoch フィールドは存在しない。架空の同一 epoch 値などを捏造せず、同一一覧走査・呼び出し側直列化・借用値の寿命規則（hold/drop）を厳格に順守する。次の一覧呼び出しや `dtv_wait_event` を重ねることはしない。

## フィールド仕様（L3 公開 getter との全 29 欄マッピング）

本仕様では、L3 公開 getter から収集される全 29 欄を以下のように規定する。

| # | JSON フィールド | 型・許容値 | L3 公開 API / 供給元 | 変換・契約規則 |
|---|---|---|---|---|
| 1 | `schema.name` | 文字列 (`"userland.discovery"`) | 固定値 | L2 §9 固定仕様 |
| 2 | `schema.major` | 数値 (`1`) | 固定値 | UDTV 1.1 仕様 |
| 3 | `schema.minor` | 数値 (`0`) | 固定値 | UDTV 1.1 仕様 |
| 4 | `producer.product` | 文字列 | `dtv_list_models()->product` | L3 モデル宣言メタデータ。空一覧時も取得可能 |
| 5 | `producer.version` | 文字列 | `config->version` | ビルド時 `PRODUCT_VERSION`（L3 決定 62 例外） |
| 6 | `scan.complete` | 真偽値 (`true`/`false`) | `dtv_get_scan_complete()` | 直前走査の完了フラグ。失敗一覧からは JSON を作らない |
| 7 | `scan.issues` | 配列 (`[]`) | 固定空配列 | 現行 L3 に問題供給元はなく常に `[]`（L3 決定 65） |
| 8 | `enclosures` | オブジェクト配列 | `dtv_list_enclosures()` | 全 capacity 枠走査。NULL 枠スキップ。0 件は `[]` |
| 9 | `enclosures[].id` | 数値 (`uint32`) | `dtv_get_enclosure_id()` | 筐体識別子 |
| 10 | `enclosures[].model` | 文字列 / `null` | `dtv_get_model()` | 公開モデル名。内部 ID は出力しない |
| 11 | `enclosures[].serial` | 文字列 / `null` | `dtv_get_enclosure_serial()` | 現候補は NULL（serial backend は別作業・未実装） |
| 12 | `enclosures[].serial_unique` | 真偽値 / `null` | `dtv_get_serial_unique()` | tri-state（YES: `true`, NO: `false`, UNKNOWN: `null`） |
| 13 | `enclosures[].status` | 文字列 | `dtv_get_status()` | 語彙: `ready`, `duplicate`, `invalid_observation`, `unknown` |
| 14 | `enclosures[].lnb_15v_supported` | 真偽値 / `null` | `dtv_get_enclosure_support(DTV_OP_LNB_POWER)` | tri-state。静的能力（動的給電状態とは別） |
| 15 | `enclosures[].card_reader_supported` | 真偽値 / `null` | `dtv_get_enclosure_support(DTV_OP_CARD)` | tri-state。ホスト外部利用可否（内部利用機種は `false`） |
| 16 | `enclosures[].card_reader_device` | 数値 / `null` | `dtv_get_card_reader_device()` | 親 USB ファンクション ID。存在しない/不明時は `null` |
| 17 | `enclosures[].devices` | オブジェクト配列 | `dtv_get_devices()` | NULL 終端配列。0 件は `[]` |
| 18 | `enclosures[].candidates` | オブジェクト配列 | `dtv_get_candidates()` | 重複代替観測。平坦 5 鍵（L3 決定 64）。0 件は `[]` |
| 19 | `devices[].id` | 数値 (`uint32`) | `dtv_get_device_id()` | 親 USB ファンクション ID |
| 20 | `devices[].usb` | 文字列 | `dtv_get_vid()`, `dtv_get_pid()` | 小文字 4 桁:4 桁 `VID:PID`（例: `"0511:084a"`） |
| 21 | `devices[].serial` | 文字列 / `null` | `dtv_get_device_serial()` | 現候補は NULL（serial backend は別作業・未実装） |
| 22 | `devices[].bus` | 数値 / `null` | `dtv_get_bus()` | 非 NULL 時数値、未観測時 `null`（数値 0 置換禁止） |
| 23 | `devices[].address` | 数値 / `null` | `dtv_get_address()` | 非 NULL 時数値、未観測時 `null`（数値 0 置換禁止） |
| 24 | `devices[].port` | 文字列 / `null` | `dtv_get_port()` | 物理トポロジ文字列（例: `"1-2.3"`） |
| 25 | `devices[].receivers` | 配列 / 省略 | `dtv_get_receivers()` | 構成未知時は**鍵を省略**、既知 0 件時は `[]` |
| 26 | `receivers[].receiver` | 数値 (`uint32`) | `dtv_get_index()` | 筐体内で一意な受信機番号 |
| 27 | `receivers[].device` | 数値 (`uint32`) | 親 `dtv_get_device_id()` | 親 USB ファンクション ID |
| 28 | `receivers[].local` | 数値 (`uint32`) | `dtv_get_local()` | 親デバイス内ローカル番号 |
| 29 | `receivers[].systems` | 文字列配列 | `dtv_get_systems()` | bit 写像（`"ISDB-T"`, `"ISDB-S"`）。未知 bit は無視 |
| 30 | `receivers[].ts_scrambling` | 文字列 / `null` | `dtv_get_ts_scrambling()` | 語彙: `"scrambled"`, `"descrambled"`, 不明時 `null` |
| 31 | `candidates[].device` | 数値 (`uint32`) | `candidate->device` | 候補 slot 番号（確定 device ID への置換禁止） |
| 32 | `candidates[].serial` | 文字列 / `null` | `candidate->serial` | 候補シリアル番号 |
| 33 | `candidates[].bus` | 数値 / `null` | `candidate->bus` | 候補 USB バス番号（平坦鍵、L3 決定 64） |
| 34 | `candidates[].address` | 数値 / `null` | `candidate->address` | 候補デバイスアドレス（平坦鍵、L3 決定 64） |
| 35 | `candidates[].port` | 文字列 / `null` | `candidate->port` | 候補物理ポート（平坦鍵、L3 決定 64） |
| 36 | `ungrouped_usb_devices` | オブジェクト配列 | `dtv_list_ungrouped_devices()` | 筐体を成さなかった孤立デバイス。0 件は `[]` |
| 37 | `ungrouped_usb_devices[].serial` | 文字列 / `null` | `ungrouped->serial` | 現候補は NULL（serial backend は別作業・未実装） |
| 38 | `ungrouped_usb_devices[].model` | 文字列 / `null` | `ungrouped->model` | モデル名（未確定時 `null`） |
| 39 | `ungrouped_usb_devices[].usb` | 文字列 | `ungrouped->vid`, `pid` | 小文字 4 桁:4 桁 `VID:PID` |
| 40 | `ungrouped_usb_devices[].status` | 文字列 | `ungrouped->status` | 語彙: `usable`, `unsupported`, `invalid_serial` 等 |
| 41 | `ungrouped_usb_devices[].bus` | 数値 / `null` | `ungrouped->bus` | 孤立 USB バス番号（平坦鍵、L3 決定 64） |
| 42 | `ungrouped_usb_devices[].address` | 数値 / `null` | `ungrouped->address` | 孤立デバイスアドレス（平坦鍵、L3 決定 64） |
| 43 | `ungrouped_usb_devices[].port` | 文字列 / `null` | `ungrouped->port` | 孤立物理ポート（平坦鍵、L3 決定 64） |

※ 上記の 43 行は、共通親オブジェクトおよび位置情報の詳細展開を含む。L3 getter から直接射影・収集される論理入力単位としては全 29 項目で構成される。

### 位置情報と平坦化規則（L3 決定 64）

正本 L3 決定 64 に従い、候補オブジェクト（`candidates[]`）および孤立デバイスオブジェクト（`ungrouped_usb_devices[]`）における位置情報（bus/address/port）は、`location` などの入れ子オブジェクトを作らず、親オブジェクト直下に平らに配置する。
- `candidates[]` は `{device, serial, bus, address, port}` の平坦な 5 鍵とする。
- `ungrouped_usb_devices[]` は `{serial, model, usb, status, bus, address, port}` の平坦な 7 鍵とする。
- `bus` および `address` は、L3 getter が非 NULL を返す場合は数値（`uint32` 型域）、NULL の場合は JSON の `null` とする（未観測を数値 0 などで代用してはならない）。`port` は文字列または `null` とする。

### 受信機の未知省略と 0 件配列

- `dtv_get_receivers(device)` の戻り値が `nullptr` の場合、当該デバイスの受信機構成は未判明（未知）であり、JSON 出力において `receivers` **鍵自体を省略**する。
- 戻り値配列の先頭要素が `nullptr`（要素数 0）の場合、受信機が存在しない（0 件）ことが確認済みであり、空配列 `[]` を出力する。

## 内部エンコーダの契約（`discovery_json_size` / `encode_discovery_json`）

内部 C++ API `include/dtv/internal/discovery-json.hpp`（名前空間 `dtv::internal`）は以下の契約を提供する。

1. **2 段階処理（サイズ計測とエンコード）:**
   - `discovery_json_size(input, &bytes)`:
     入力データ全体の UTF-8 検証、ID 重複チェック、列挙値検証、および末尾改行（`\n`）を含む正確な byte 数を計測する。
   - `encode_discovery_json(input, output)`:
     出力を `output->data` へ書き込み、実際に書き込んだバイト数を `output->size` に設定する。
2. **失敗時の無変更保証:**
   - 入力検証失敗（`invalid_argument`）または出力先バッファ容量不足（`buffer_too_small`）が発生した場合、エンコーダは出力先バッファの内容を一切変更せず、`output->size` も変更しない。
3. **追加ターゲット非新設:**
   - 本エンコーダは Core ライブラリの内部実装として組み込まれ、出力専用の外部バイナリターゲット等は新設しない。

## CLI `--list-json` の呼び出し・出力・終了規則

1. **引数優先順位:**
   - 生成 CLI において、引数は `--help` (`-h`) → `--version` → `--list-json` → `--models` の順に優先して評価される。
   - `--list-json` は単独実行のほか、他の不正引数チェックよりも優先して実行される（`--models` のように他引数との併用を理由とする即時拒否 `argc != 2` 規則はない）。未知の引数のみが渡された場合は exit 2 となる。
2. **出力と stdout 失敗の区別:**
   - 成功時は compact UTF-8 JSON 1 文書と末尾改行（`\n`）1 個を stdout へ書き出し、exit 0 で正常終了する。
   - 内部検証失敗、L3 走査失敗、メモリ不足等の内部失敗時は、stdout への出力を一切開始せず（stdout 空）、stderr にエラーを出力して exit コード（L3 エラー写像、または 70）で終了する。
   - 一方、完成したバッファを stdout へ書き込む際の物理的な書込み失敗（`fwrite` 失敗または `fflush` 失敗）が発生した場合は、prefix 回収不能として exit 70（EX_IOERR）とする。内部失敗時の「部分出力なし（stdout 空）」と、物理 I/O 障害時の途中失敗を明確に区別する。

## 接点および未接続・未実装境界

- **Serial Backend:**
  - 筐体・デバイス・孤立デバイスのシリアル番号（`serial`）は現行実装ではすべて NULL、`serial_unique` は UNKNOWN（`null`）のまま L3 getter から写像される。
  - 本 PR は全 29 欄のスキーマ定義および CLI 接続の完了であり、serial backend 自体の完成ではない（別作業として後続実装される）。架空の空文字列や `false` を捏造しない。
- **容量超過と scan.issues（L3 決定 65）:**
  - 一覧の最大容量（既定 128 筐体）を超過して筐体が除外された場合、Core 内部の警告ログ出力および `scan.complete = true` の維持にとどまり、`scan.issues` には載せない（常に `[]`）。
  - 未決の `scan.issues` コード（`capacity_omitted` 等）や公開 getter（`dtv_get_scan_omitted_count` 等）は採用しない。
- **後続コンポーネント:**
  - 別プロセスの常駐 daemon 統合や CLI daemon 動作は本 PR には含まれず、未実装のままである。
