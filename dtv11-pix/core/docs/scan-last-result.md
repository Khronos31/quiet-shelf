<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# 直前走査結果の共有（孤立デバイスおよびスキャン完了状態）仕様

本文書は、正本 L2 §9、L3 §4.1/4.5、および L3 決定 62（正本 `ece223d0`）に基づく、直前の一覧関数が実行した走査結果（孤立 USB デバイスおよびスキャン完了状態）の共有仕様（PR75）の実装利用境界および外部契約を記録したものである。

## 概要と位置づけ

- **目的:** CLI `--list-json` などにおいて、1 回の走査バッチから完全な筐体一覧、孤立 USB デバイス一覧、およびスキャン完了フラグを整合して取得可能とするため、直前の一覧関数が実行した走査結果を getter 間で共有する。
- **公開境界:** 新たな公開 API（`dtv_visit_scan` や件数 getter 等）は追加せず、既存の公開 API（`dtv_list_ungrouped_devices`、`dtv_get_scan_complete`）の内部挙動を「再走査を行わず直前の走査結果を返す」契約へと整合させる。

## 走査結果共有と呼び出し順序・寿命契約

1. **呼び出し順序（一覧先行順）:**
   - 呼び出し側は、まず一覧関数（`dtv_list_enclosures`、`dtv_list_enclosures_for_product`、`dtv_list_enclosures_by_fds`、`dtv_list_enclosures_by_tokens` のいずれか）を実行し、その直後に `dtv_list_ungrouped_devices` および `dtv_get_scan_complete` を呼び出す。
   - `dtv_list_ungrouped_devices` および `dtv_get_scan_complete` は、自身で USB 再列挙、token callback、FD wrap、mode probe、registry 更新を起こさず、直前の一覧関数が観測したバッチの共有結果をそのまま返却する。
2. **走査バッチの置換と FD / token 契約:**
   - 成功した新しい一覧呼び出しによってキャッシュが更新・置換される。getter（`dtv_list_ungrouped_devices`、`dtv_get_scan_complete`）の連続呼び出し自体によって観測が進むことはない。
   - ただし、次の一覧呼び出しや `dtv_wait_event` によるイベント処理では抜去認識や再観測が進むため、旧孤立デバイスポインタは寿命境界外となる（L3 §4.3/4.5。孤立デバイス型（`dtv_ungrouped_device`）に対する hold 手段は存在せず、必要な値は次の一覧関数呼び出しまたは `dtv_wait_event` の寿命境界より前にコピーまたは消費する）。
   - FD 一覧（`dtv_list_enclosures_by_fds`）の成功時も、当該 FD 観測バッチの孤立デバイスで置換され、以前の通常走査の孤立デバイスを残さない。
   - 製品別一覧や token 一覧（`dtv_list_enclosures_by_tokens`）においても、直前の走査バッチに由来する孤立デバイスと完了状態を共有し、既存の筐体選択範囲、FD 所有権、オブジェクト同一性、筐体寿命規則（hold / drop）は一切変更しない。
3. **失敗 3 区分とキャッシュの挙動:**
   - **初回呼び出し前:** `complete = 0`、孤立デバイスなし。
   - **実際に走査を実行した失敗:** USB 走査や内部メモリ確保等の実走査失敗時は、`complete = 0`、孤立デバイスなしとなり、前回の成功値を今回の成功結果として返さない。
   - **未走査の入力検証失敗および token 選択未確定:** 引数不正（NULL、不正 FD、不正 token 配列等）により走査を開始せずに終了した場合、または走査後に token 選択で該当なしとなった場合は、直前の成功キャッシュを維持する（未走査失敗による強制破棄の新契約は追加しない）。
   - ※ 走査失敗時に getter が NULL または空配列のいずれを返すべきかという議論は所有者未決の検討事項であり、本仕様では普遍的な正本契約として規定しない。

## 接点および未接続境界

- **容量超過の扱い:** 一覧の最大容量（既定 128 筐体）を超過した場合の挙動は、既存の警告ログ出力および `scan_complete = true` の維持にとどまり、未決の `scan.issues` コード（`capacity_omitted` 等）の創出や採用は本 PR には含めない。
- **未実装・後続境界:** 本 PR は Core 内部の一覧走査結果共有のみを対象とし、CLI `--list-json` の完全な JSON 直列化接続、新規公開 API、別プロセス daemon 統合は後続工程の責務である。
