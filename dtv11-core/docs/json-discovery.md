<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# userland.discovery JSON 出力仕様

ヘッダーファイル `discovery_json.h` は、ハードウェアスキャン全体の観測結果を、公開仕様で定義された `userland.discovery` JSON 形式へとマッピング（変換出力）する。単一の `ListResponse` ではなく、スキャン全体の包括的な観測データ（scan envelope）を入力として受け取るため、同一スキャン内で観測された全デバイスにわたり、筐体のグルーピングおよびシリアル番号の一意性を整合性高く判定する。

出力される JSON は、以下のとおり実機の物理的な階層構造を保持する。

```json
{
  "schema": {"name": "userland.discovery", "major": 1, "minor": 0},
  "producer": {"product": "px4", "version": "1.0.0"},
  "scan": {"complete": true, "issues": []},
  "enclosures": [{
    "id": 1,
    "model": "PX-Q3U4",
    "serial": "serial-value",
    "serial_unique": true,
    "status": "ready",
    "lnb_15v_supported": null,
    "card_reader_supported": true,
    "card_reader_device": 2,
    "devices": [{
      "id": 2,
      "usb": "0511:084a",
      "serial": "usb-serial",
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
    "candidates": []
  }],
  "ungrouped_usb_devices": []
}
```

識別子（ID）は JSON の数値として出力する。VID および PID は小文字の 16 進数表記による `VID:PID` 文字列とする。受信機構成が未知のデバイスでは `receivers` フィールドを省略し、受信機が 0 個であることが確認されているデバイスでは空配列（`[]`）を出力する。本エンコーダ（`discovery_json`）はスキャン処理によって実際に提供された筐体およびデバイスのみを出力し、架空の要素を補完することはない。

## フィールド仕様

### ルートオブジェクト

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `schema.name`, `schema.major`, `schema.minor` | 文字列、数値、数値 | `"userland.discovery"`、`1`、`0` |
| `producer.product`, `producer.version` | 文字列、文字列 | 製品名（例: `"px4"`）およびバージョン文字列 |
| `scan.complete` | 真偽値 | ハードウェアスキャンが完全に終了したかを示すフラグ |
| `scan.issues` | オブジェクト配列 | スキャン中に検出された問題のリスト。固定エラーコード文字列と詳細メッセージを保持する |
| `enclosures` | オブジェクト配列 | 観測・識別された筐体オブジェクトの配列 |
| `ungrouped_usb_devices` | オブジェクト配列 | 特定の筐体に割り当てられなかった孤立 USB デバイスの配列 |

### 筐体オブジェクト（`enclosures[]`）

筐体の `id` および USB ファンクションの `id` は、STATUS JSON における各識別子と同じセマンティクスを持つ。

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `id` | 数値 | 筐体識別子 |
| `model` | 文字列 / `null` | 公開モデル名文字列（形式例: `"PX-Q3U4"`）。内部 ID は出力しない |
| `serial` | 文字列 / `null` | 筐体シリアル番号文字列 |
| `serial_unique` | 真偽値 / `null` | 今回のスキャンで観測された筐体間におけるシリアル番号の一意性指標（後述） |
| `status` | 文字列 | デバイスのグルーピング状態（`ready`、`incomplete`、`duplicate`、`invalid_observation`、`unknown`） |
| `lnb_15v_supported` | 真偽値 / `null` | 筐体の静的な LNB 15V 給電能力。不明時は `null`。動的給電状態（`lnb_15v_enabled`）とは区別される |
| `card_reader_supported` | 真偽値 / `null` | ホストからカードリーダーを扱えるか（スマートカード関連操作が可能か）を示す静的能力指標。対応時は `true`、非対応時は `false`、ファームウェア転送前など未確定・不明時は `null` |
| `card_reader_device` | 数値 / `null` | カードリーダーが物理的に属する親 USB ファンクション（デバイス）の `id`。リーダーが存在しない場合や不明時は `null` |
| `devices` | オブジェクト配列 | 筐体に属する USB ファンクション（デバイス）の配列 |
| `candidates` | オブジェクト配列 | 重複観測における代替 USB 観測データ配列 |

#### カードリーダーの公開仕様
旧仕様における `readers[]` 配列およびリーダー番号（0, 1 など）の公開は廃止された。カードリーダーは筐体ごとに 0 個または 1 個として扱われ、JSON 上にリーダー番号は露出しない。
ホストからの外部リーダー利用可否（`card_reader_supported`）と、機器内部でのカード利用（例: DT181 などの内部カード処理）やカード挿入有無（card presence）は別概念である。DT181 のような内部カード利用機種は外部リーダー非対応（`card_reader_supported: false`、`card_reader_device: null`）として扱われる。

#### シリアル番号の一意性（`serial_unique`）
`serial_unique` は、デーモン起動時に該当筐体をどのように特定・選択すべきかを示す指標である:
- `true`: `--device serial:…` のシリアル番号指定によって一意に選択可能であることを意味する。
- `false`: 同一シリアルを持つ別筐体の存在やシリアル競合により指定が曖昧となるため、USB 物理バスパスを指定する `--device usb:…` を使用すべきであることを意味する。
- `null`: シリアル番号自体が観測されなかったか、あるいはスキャンが不完全な状態で終了し重複の有無を確定できなかったことを意味する。

実際にシリアル番号が観測された場合、たとえ不完全なスキャンであっても、重複状態が検出されたり一致する別シリアルが確認された段階で直ちに `false` となる。重複のない観測シリアルが `true` と確定するのは、スキャン全体が完全に完了した後に限られる。この一意性は「今回のスキャンで観測された筐体間」における相対的な保証である。

#### 重複候補（`candidates`）
`candidates` は重複した筐体観測に対する代替の USB 観測データを保持するが、それらが子デバイスとして確定したことを意味しない。各候補データはデバイススロット番号、null 許容のシリアル番号、および null 許容の bus/address/port 情報を保持する。候補（candidates）レコード内の `device` は、重複観測における旧来のデバイススロット番号であり、筐体 ID ではなく、また候補が直属の子デバイスであることを主張するものでもない。

### デバイスオブジェクト（`enclosures[].devices[]`）

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `id` | 数値 | 親 USB ファンクション ID |
| `usb` | 文字列 | 小文字 16 進数表記による `VID:PID` 文字列 |
| `serial` | 文字列 / `null` | USB デバイスのシリアル番号文字列 |
| `bus`, `address` | 数値 | USB バス番号およびデバイスアドレス |
| `port` | 文字列 / `null` | USB 物理ポートトポロジ文字列（例: `"1-2.3"`） |
| `receivers` | オブジェクト配列 | デバイスが収容する受信機（チューナー）の配列。構成未知時は省略、0 個確認時は `[]` |

### 受信機オブジェクト（`devices[].receivers[]`）

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `receiver` | 数値 | 同一筐体内全体を通じて重複なく一意な受信機番号 |
| `device` | 数値 | その受信機を収容している親 USB ファンクション ID |
| `local` | 数値 | 親デバイス（USB ファンクション）内部でのローカルインデックス |
| `systems` | 文字列配列 | 受信機が対応する放送方式の配列（例: `["ISDB-T"]`、`["ISDB-T", "ISDB-S"]`） |
| `ts_scrambling` | 文字列 / `null` | 受信部が出力する放送 TS のスクランブル状態（後述） |

#### TS スクランブル状態（`ts_scrambling`）
`ts_scrambling` は、受信機が出力する TS のスクランブル状態を示す:
- `"scrambled"`: スクランブルされた生の放送 TS を出力する。B-CAS 処理（B25）は利用者が行う。
- `"descrambled"`: 機器内部でスクランブルを解除した TS を出力する（例: PIX 公式ファームウェア運用時）。
- `null`: ファームウェア転送前、または状態が判明する前（不明時）。

本フィールドは製品変換後の放送 TS のスクランブル状態を表すものであり、USB 転送路の保護（USB AES 等）の有無は含まない（AES 暗号化は製品ライブラリ内部で透過的に解除される）。

### 孤立 USB デバイス（`ungrouped_usb_devices[]`）

特定の筐体に割り当てられなかった孤立 USB デバイスは `ungrouped_usb_devices` に留まり、`serial`（null 許容）、`model`（null 許容）、`location`（null 許容）、`usb`（小文字 `VID:PID`）、およびデバイス状態（`usable`、`unsupported`、`invalid_serial`、`insufficient_speed`、`invalid_topology`、`open_failed`、`unknown` のいずれか）を出力する。

## エンコーダの出力保証

`discovery_json_size` は入力データ全体を検証した上で出力電文のバイトサイズを正確に計測する。エンコーダは実際の書き込みに先立ち完全な検証とサイズ計測を実行し、出力先バッファが不足している場合は `BUFFER_TOO_SMALL` を返し、いずれの失敗時にも出力先バッファの内容を変更しない。この API は `dtv::json` ライブラリの一部として提供され、出力専用の追加バイナリターゲットを新設することはない。スキャン結果の配列は借用されたポインタと要素数のペアとして渡されるため、アダプター層は固定の検出件数上限に縛られることなく観測された全筐体データを引き渡すことができる。関数の実行期間中、ポインタが指し示す入力配列の内容を変更してはならない。
