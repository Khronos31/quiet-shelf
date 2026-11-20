<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# UDTV 1.1 LNB_POWER ペイロードコーデック仕様

本文書は、正本 L2 仕様（`spec/dtv11-l2-spec.md` §1/3/4/7、正本 `93d2948d` 決定 60）および凍結外部 byte 契約（`docs/wire-records.md:19`、`include/dtv/userland/records.h:491-499`）に基づく、UDTV 1.1 上の LNB_POWER 要求/応答 payload codec（PR69 / PR73）の実装利用境界および外部契約を記録したものである。

## 概要と位置づけ

- **積層関係:** PR67 / PR73（20 byte UDTV 1.1 envelope codec および増分 `FrameReceiver`）および PR68 / PR73（UDTV 1.1 固定長 typed payload codec）の上に積層された、LNB 電源制御メッセージの直列化・非直列化層である。
- **公開境界:** 公開 L3 API（`include/dtv/dtv.h`）は変更せず、Core 内部専用ヘッダー（`include/dtv/internal/wire-lnb-power.hpp`）および実装（`src/wire-lnb-power.cpp`）に閉じる。
- **特性:** 動的メモリ確保なし、I/O なし、時計なし。呼び出し側提供領域の借用規約（`dtv_bytes`, `dtv_buffer`）をそのまま使用する。

## LNB_POWER 固定長 17 byte 仕様（両方向共通配置）

### 1. メッセージ識別およびエンベロープ
- **opcode:** `0x0039` (`WireOpcode::lnb_power`)
- **エンベロープ flags:**
  - 要求: `flags = 0` (REQUEST)
  - 応答: `flags = 1` (RESPONSE)
- **request_id:** 非 0（クライアント割当の相関識別子）
- **外郭バージョン:** UDTV 1.1（major: 1, minor: 1）

### 2. ペイロード構成（要求・応答共通）
- **ペイロード長:** 要求・応答とも正確に 17 byte（UDTV 1.0 / 1.1 共通配置）。
- **フィールド配置:**
  - `epoch`: `std::uint8_t[16]` (offset 0..15、16 byte 不透明データとしてそのままコピー)
  - `enabled`: `std::uint8_t` (offset 16、`0` = 電源 OFF, `1` = 電源 ON)
- **不透明性:**
  - `epoch` は不透明値としてそのまま往復し、全 0 の epoch も透過する。codec 側で epoch の現在性やセッション照合は行わない。

## 入力・出力規約と無変更失敗保証

- **デコード (`decode_lnb_power`):**
  - ペイロード長が正確に 17 byte でない場合（16 byte 以下または 18 byte 以上）、`WireFrameCode::protocol_error` を返却。
  - `enabled` フィールドが `0` または `1` 以外（例: `2` や `0xFF` 等）の値を含む場合、`WireFrameCode::invalid_argument` を返却。
  - 不正 view（null ポインタ等）は `WireFrameCode::invalid_argument` を返却。
- **エンコード (`encode_lnb_power`):**
  - 出力先バッファの容量（`dest->capacity`）が 17 byte 未満の場合、`WireFrameCode::buffer_too_small` を返却し、**出力先バッファの内容および `dest->size` を一切変更しない（無変更失敗保証）**。
  - `value->enabled` が `0` または `1` 以外の場合、`WireFrameCode::invalid_argument` を返却。
  - 成功時のみ 17 byte を書き込み、`dest->size = 17` を設定。
  - 構造体のメモリ表現（raw struct）を直接流用せず、1 byte ずつ安全に読み書きする。

## FrameReceiver との接点および未接続・後続境界

- **FrameReceiver（PR67 / PR73）との接点:**
  - クライアント送信時: 将来 STATUS 等で取得した接続 epoch と希望する電源状態（`enabled` 0/1）を本 codec の `encode_lnb_power` で 17 byte に直列化し、PR67 / PR73 `write_header`（major 1, minor 1, opcode `0x0039`、flags 0、request_id 非 0）により 1.1 要求フレームを生成する。
  - デーモン受信時: `FrameReceiver` が完成させたフレームの opcode（`0x0039`）および payload 長を確認後、本 codec の `decode_lnb_power` へ渡して型付き構造体を取得する。旧 1.0 外郭ヘッダーを受信した場合、`FrameReceiver` は 20 byte ヘッダー時点で `WireFrameCode::version_mismatch` を返して sticky error 状態となり、payload を読む前に接続を終了・切断するのは呼び出し側（host runtime）の責務である（`FrameReceiver` 自身は socket/I/O/close を持たない）。
  - デーモン応答時: 応答状態を本 codec で 17 byte に直列化し、PR67 / PR73 `write_header`（major 1, minor 1, opcode `0x0039`、flags 1、同 request_id）により 1.1 応答フレームを生成して返信する。
- **未実装・未接続・後続境界:**
  - **実際の給電・機器制御:** 実際の LNB 電源制御（Core L3 `dtv_lnb_on`/`off` や製品 SPI へのディスパッチ）、機器の LNB 給電能力判定は未接続。
  - **接続 gate / 権限:** クライアントの操作権限検査、接続 gate でのセッション検証、epoch の現在性照合、要求と応答の整合性判定は上位 host runtime の責務であり、本 codec には含めない。
  - **タイマー:** 5 秒未完了タイムアウト判定は接続を保持する host runtime の責務（本 codec は時計・タイマーを持たない）。
  - **全 A2 未接続:** 本 PR73 は envelope / HELLO 版整合（UDTV 1.1）のみであり、全 A2 の error 意味送受信、epoch 回復、detail 送信、接続 gate、client、daemon 完成とは主張しない。正本 L2 §7 / 決定 59 で error payload 配置は既決、決定 60 でエラーコード 1..22 および版 1.1 も既決であるが、現 main / PR73 に codec や gate は未実装である。opcode ごとの operation 文字列名表のみ未採用。
  - **未実装 payload / トランスポート:** error payload codec、他 typed record、JSON、CLI、daemon host runtime、IPC transport は現 main / PR73 では未実装・未接続。
  - **L2 未決事項:** STATE_EVENT 追加 feature bit、HELLO 上限比較、TS_DATA 0 packet は未決定・未変更（※ error payload 配置は L2 §7 / 決定 59 で既決であり未決ではない）。
