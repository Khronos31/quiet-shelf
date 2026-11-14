<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# 宣言基盤・C ABI・CMake登録の使い方と到達範囲

本文書は、Coreリポジトリに導入された宣言基盤、C ABI、CMake製品登録（`dtv_product`）、内部scan/group観測、および公開探索・寿命・イベント待機APIの仕様と利用方法を、後続のCoreおよび製品実装者向けに説明するものである。

## 3層構成と生成物の区分

本アーキテクチャでは、呼び出し経路と責務を以下の3層に明確に分離している。

1. **Core共通層（`dtv_*`）:** 共通の公開C ABI（`include/dtv/dtv.h`）。内部ディスパッチテーブルおよび共有探索レジストリを持ち、登録された製品・機種の操作や公開探索APIを提供する。
2. **製品層（`dtv_*_<product>`）:** CMakeが生成するC ABIラッパーおよび登録エントリ。手書き製品コードとCoreを仲介する。
3. **機種層（`dtv_*_<product>_<model>` / `dtv_model_impl_*`）:** 機種ごとの手書き実装関数。機能宣言に応じた処理を実装する。

ビルド時に生成されるレジストリ（`${PRODUCT_NAME}_registry.cpp`）、CLI main（`${PRODUCT_COMMAND}_main.cpp`）、アセットヘッダ（`${PRODUCT_NAME}_firmware.hpp`）、および未宣言操作のstubは、上記の手書きコードとは区別されるビルド生成物である。

なお、現時点では**筐体・受信機の open / claim、実機操作ランタイム（USB I/O、TS処理、tune等）、および製品実装は未接続・未実装**である。公開ヘッダに宣言が存在しても、全シンボルが実装・解決済みではない点に注意すること。

## 機能宣言と型検査（C++20 SPI）

製品および機種の機能は、`include/dtv/spi/feature.hpp` のマクロを用いて宣言する。

- **機能一覧（`dtv::spi::Feature`）:** `LnbPower`, `Card`, `OutputPidFilter`, `TuneIsdbT`, `TuneIsdbS`, `SelectSlot`, `SelectTsid`, `Signal`, `Lna` の9種（ファームウェア送信は通常機能ではなく独立したFW-or-none宣言で扱う）。
- **製品機能宣言（`DTV_PRODUCT`）:** 製品がサポートしうる通常機能集合を `DTV_FEATURES(...)` で定義する。
- **機種機能宣言（`DTV_MODEL`）:** 機種がサポートする通常機能集合、ファームウェア宣言、基本情報を定義する。機種の通常機能は製品機能の部分集合（subset）である必要があり、違反時は `static_assert` によりコンパイルエラーとなる。
- **ファームウェア宣言:** `DTV_NO_FIRMWARE` または `DTV_FIRMWARE(asset_name)` の指定が必須である（決定36/43。送信対応の唯一の根拠であり、独立したFirmware feature bitは不要）。未指定時は `static_assert` で検出される。
- **モデル定義（`DTV_DEFINE_MODEL`）:** 機種の公開操作テーブルと内部レコード（`l3_model_record`）を生成する。このマクロは、既存の private な `ModelEntry`（`dtv::spi::<model>_model`）へのポインタを `l3_model_record::discovery` として強参照・結合する。CMake生成の `dtv_l3_register_product` は同じ `MODELS` 列由来の `ProductEntry` をCoreへ渡し、L3モデルレコードとの位置対応を検証・保持する。USBグループ判定（`select_usb_group`）は各機種の `ModelEntry` が担う単一の機種責務であり、宣言だけで `ModelEntry` が不要になるわけではない（Core側にハードウェア機種表は増やさない）。

```cpp
// tests/fixture/lib/fixture_one.cpp より引用
DTV_PRODUCT(fixture, DTV_FEATURES(::dtv::spi::Feature::LnbPower));
DTV_MODEL(fixture, fixture_one, DTV_FEATURES(::dtv::spi::Feature::LnbPower), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"Fixture One", 0x1234, 0x0001}));
DTV_DEFINE_MODEL(fixture, fixture_one);
```

## 8操作行集合・未宣言stub・強参照リンクエラー

正本に対応する公開モデル操作は、`DTV_PUBLIC_MODEL_OPS` で定義される以下の8操作である。

1. `lnb_on`（要求機能: `feature_set<Feature::LnbPower>`）
2. `lnb_off`（要求機能: `feature_set<Feature::LnbPower>`）
3. `tune_by_frequency`（要求機能: `feature_set<Feature::TuneIsdbT>`。内部判定により `TuneIsdbT` または `TuneIsdbS` のいずれかで可）
4. `get_signal`（要求機能: `feature_set<Feature::Signal>`）
5. `set_pid_filter`（要求機能: `feature_set<Feature::OutputPidFilter>`）
6. `send_firmware`（要求機能: `feature_set<>`。通常機能集合ではなくFW-or-none宣言で判定）
7. `send_default_firmware`（要求機能: `feature_set<>`。通常機能集合ではなくFW-or-none宣言で判定）
8. `reset_card`（要求機能: `feature_set<Feature::Card>`、引数: `(dtv_enclosure *enclosure, const dtv_call_options *options, std::vector<std::uint8_t> *out, dtv_error *err)`）

### 未宣言stub・FW成功・強参照
- **通常操作の未宣言stub:** 機種の通常機能に含まれない操作は、ラッパー内で未サポートと判定され `DTV_ERROR_UNSUPPORTED` を即座に返すstub動作となる。手書き実装関数は参照されない。
- **FW操作の判定:** ファームウェアの2操作は通常機能集合を要求せず、`DTV_NO_FIRMWARE` の場合は何もせず `DTV_ERROR_OK` で成功する（未宣言通常操作のUNSUPPORTEDとは混同しない）。`DTV_FIRMWARE(asset)` の場合は手書き送信処理を呼び出す。
- **宣言済み操作の強参照:** 機能を宣言した通常操作、および `DTV_FIRMWARE(asset)` を宣言した機種の `send_firmware` は、手書き実装シンボル `dtv_model_impl_<op>_<product>_<model>` を強参照する。このため、宣言がありながら実装を定義し忘れた場合、リンク時に未定義シンボルエラー（undefined reference）として確実に検出される。
- **責任分離:** 機種固有の電源・リセットライン制御・UART・mailbox等の物理搬送は機種関数が担当し、Core共通層は公開C ABI境界、引数・能力照会、同一筐体カード操作の直列化、内部エポック照合による遅延応答フェンス、ATRの完全性・TCK検査、およびCore内部所有領域への保存・寿命管理を担当する。

## ファームウェア資産と手書き例外

- **送信対応の唯一の根拠:** ファームウェア送信に対応するか否かは、モデルの `DTV_FIRMWARE(asset)` または `DTV_NO_FIRMWARE` 宣言のみで決まる（決定36/43）。別のFirmware feature bitを通常機能集合に指定する必要はない。
- **資産埋め込み:** CMakeで `FIRMWARE <name>=<path>` を指定すると、バイナリを埋め込んだ構造体 `dtv::spi::firmware_assets::<name>` を含むヘッダが生成される。
- **default wrapper:** 機種が `DTV_FIRMWARE(<name>)` を宣言している場合、生成される `send_default_firmware` ラッパーは埋め込みバイト列（`data`, `size`）を取り出して手書きの `dtv_model_impl_send_firmware_*` へ渡す。手書き実装が欠落している場合はリンクエラーとなる。
- **no-FW:** `DTV_NO_FIRMWARE` を宣言した機種では、ファームウェア送信操作は何もせず `DTV_ERROR_OK` を返し、手書き実装の作成は不要である。
- **手書き製品_to_all例外:** 製品全体の全機器に対する一括送信関数 `dtv_send_default_firmware_to_all_<product>`（例: `dtv_send_default_firmware_to_all_fixture`）は自動生成されない。製品側の共通ソース（`SOURCES`）で必ず手書き実装を定義しなければならない。

```cpp
// tests/fixture/src/asset_model.cpp より引用
DTV_PRODUCT(assetfixture, DTV_FEATURES());
DTV_MODEL(assetfixture, asset_model,
          DTV_FEATURES(),
          DTV_FIRMWARE(fixture_blob),
          (::dtv::spi::model_information{"Firmware Fixture", 0x1234, 0x5678}));
DTV_DEFINE_MODEL(assetfixture, asset_model);

extern "C" int32_t dtv_model_impl_send_firmware_assetfixture_asset_model(
    dtv_device *, const uint8_t *firmware, uint64_t size,
    const dtv_call_options *, dtv_error *) {
    return size == dtv::spi::firmware_assets::fixture_blob::size ? DTV_ERROR_OK : DTV_ERROR_FIRMWARE_REJECTED;
}
```

## CMake登録（`dtv_product`）

製品のCMakeLists.txtでは `dtv_product`（または `dtv_add_product`）を呼び出す。

```cmake
# tests/fixture/CMakeLists.txt より引用
dtv_product(NAME fixture
            COMMAND fixturectl
            VERSION 1.2.3
            DEFAULT_MODEL ""
            MODELS fixture_one fixture_two
            SOURCES product-to-all.cpp firmware-consumer.cpp
            FIRMWARE fixture_blob=firmware.bin)
```

- **引数一覧:**
  - `NAME`: 製品識別子（C識別子）。
  - `COMMAND`: 生成されるCLI実行ファイル名。
  - `VERSION`: 製品バージョン。
  - `DEFAULT_MODEL`: 既定機種名（空文字列可）。
  - `MODELS`: 機種識別子リスト。各識別子について `src/${model}.cpp` または `lib/${model}.cpp` が自動探索される。`SOURCES` に重複して含めてはならない。
  - `SOURCES`: 製品固有の手書き共通ソース（`_to_all` 実装など）。
  - `FIRMWARE`: `name=path` 形式のファームウェアバイナリ指定（任意）。
- **生成アーティファクト:**
  - `${PRODUCT_NAME}_api.h`: 公開Cヘッダ（ロード関数、製品別一覧関数 `dtv_list_enclosures_<product>`、製品別token選択一覧関数 `dtv_list_enclosures_by_tokens_<product>`、製品別FD選択一覧関数 `dtv_list_enclosures_by_fds_<product>`、個別メタデータ関数）。
  - `${PRODUCT_NAME}_registry.cpp`: 製品レジストリと強参照テーブル。
  - `${PRODUCT_NAME}_firmware.hpp`: ファームウェアバイト列ヘッダ（FIRMWARE指定時）。
  - `${PRODUCT_COMMAND}_main.cpp`: CLIメインエントリ。

## 明示ロード・メタデータ・サポート判定（C ABI）

公開Cヘッダ `include/dtv/dtv.h`（正本 `45cecd06` の `l3/dtv.h` に準拠）および生成APIヘッダにより、以下のC ABIを提供する（C11 / C++双方から利用可能）。

- **明示ロード:** 利用側はまず `dtv_load_<product>()` を呼び出してCoreへ製品を登録する。
- **モデル一覧:**
  - `dtv_list_models()`: ロード済み全製品のモデル配列（`const dtv_model *const *`、NULL終端）を返す。
  - `dtv_list_models_<product>()`: 当該製品に属するモデル一覧を返す。
- **サポート判定:**
  - `dtv_get_product_support(product, op, &tribool, err)`: 製品全体での通常操作サポート状況を `DTV_YES`, `DTV_NO`, `DTV_UNKNOWN`（機種によって異なる）のtriboolで返す。
  - `dtv_get_model_support(model, op, &tribool, err)`: 指定機種の通常操作サポート状況を `DTV_YES` または `DTV_NO` で返す。
  - ※注意: モデルの feature mask や support query は通常の操作能力（`DTV_OP_LNB_POWER` 〜 `DTV_OP_LNA`）のみを対象とする。公開の `DTV_OP_*` 定数にファームウェア送信の項目は存在せず、FW-or-none宣言は通常のsupport queryには二重計上されない（FW宣言のみで通常機能を持たないモデルの通常操作supportは `DTV_NO` となる）。

## 内部完全グループ観測（scan-only）

ロード済み製品の `MODELS` 宣言に基づき、caller駆動で完全なUSBグループのみを観測・抽出する内部基盤（`include/dtv/internal/discovery.hpp`, `include/dtv/internal/usb.hpp`）を提供する。

- **`UsbContext::scan_only()`:** バックグラウンドのイベント／ライフサイクルワーカーを起動せずにlibusbコンテキストを初期化する（ワーカー数0）。呼び出し元（caller）スレッド上で同期的に実行される。従来の既定コンストラクタ（2ワーカー起動経路）はそのまま温存され、新経路からは呼び出さない。
- **`collect_complete_groups(products, snapshots, out)`:** ロード済み製品ビュー（`LoadedProductDiscoveryView`）とUSBスナップショット列を受け取り、各機種の `select_usb_group` コールバックを呼び出して `DTV_USB_GROUP_COMPLETE` と判定された観測値のみを `CompleteGroupBatch` に抽出する。製品・モデルレコードの位置対応が検証される。
- **`observe_loaded_product_groups(snapshots, out)`:** Coreのロード済み製品レジストリのスナップショットを取得し、渡された `snapshots` から完全グループを観測する。
- **`scan_loaded_product_groups(out)` / `scan_loaded_product_groups(context, out)`:** `scan_only()` コンテキストを作成または受け取り、同期的にUSBデバイスを列挙して、ロード済み製品に対する完全グループを `CompleteGroupBatch` として取得する。
- **所有境界と結果の寿命:**
  - `CompleteGroupBatch` が `UsbDeviceSnapshot` のベクター（`batch.snapshots`）の所有権を持つ。
  - 各 `CompleteGroupObservation`（`batch.groups`）は、安定した `ProductEntry` / `l3_model_record` へのポインタ、`anchor_candidate_index`、および `functions`（選択された候補インデックスとinterface値）を保持する。候補インデックスはバッチ内のスナップショット配列のみを参照する。
  - 部分一致（`INCOMPLETE`）、不一致（`NO_MATCH`）、曖昧一致（`AMBIGUOUS`）は除外され、出力には含まれない。
  - 不正な選択（インデックス範囲外、存在しないinterface/alternate）、コールバック失敗（`UNSUPPORTED`等）、メモリ確保失敗（`std::bad_alloc` 例外は `RESOURCE_EXHAUSTED` に変換）が発生した場合は失敗コードを返し、出力バッチは空となる。
  - `scan_only` モードでは、descriptor取得エラーや負のport path取得時は部分的なスナップショットを黙って返さず列挙失敗とする。

## 内部共有レジストリとイベントジャーナル（EnclosureRegistry / DiscoverySession）

PR24では、継続的なスキャンセッション、固定スロット管理、root/childオブジェクト寿命、およびイベントジャーナルを管理する内部基盤（`include/dtv/internal/l3-registry.hpp`）を追加している。

- **継続 `DiscoverySession` と接続token:**
  - `DiscoverySession` は単一の永続 `UsbContext::scan_only()`（ワーカー数0）を所有し、`refresh()` により内部スキャンと `EnclosureRegistry::refresh` を駆動する。
  - 走査内の一時的な `candidate_id`（1〜N）や、USBの `bus` / `device_address` / `port_path` は同一接続の判定キーには使用しない。保持している `libusb_device` 参照ごとにセッション内で単調増加する接続トークン（`connection_token`）を発行・管理する。
  - 再走査によりデバイスの列挙順序が変わっても、同じUSBデバイス参照が保持されている限り同一の接続トークンとなり、同一のグループキー（`ProductEntry` + `l3_model_record` + ソート済み接続トークン列）として認識され、同一の root / child / 固定スロットを維持する。
  - 機器が抜去されて再挿入された場合は新しい `libusb_device` 参照となるため、新しい接続トークンが割り当てられ、新しい root オブジェクトとして生成される。
- **`EnclosureRegistry` の固定live配列と回収境界:**
  - `live_slots()` は固定長配列を返す。配列の容量は既定128で、ビルド時の `DTV_MAX_ENCLOSURES` で変更可能であり、その設定値が固定配列容量となる（決定18/47）。設定容量を超える完全筐体は一覧から除外され、事前登録した既存の `dtv_set_log_callback` へ除外数と設定容量が通知される（コールバックの既定値はnullであり、未登録時は出力されず、stderrへの出力も行わない）。抜去された筐体のスロットは `nullptr` となり、配列の後ろ詰めは行わない（穴非圧縮。他筐体のスロット位置は動かない）。
  - root（`dtv_enclosure`）は安定した子 `dtv_device` オブジェクト群およびそのスナップショット参照を所有する。
  - 抜去された筐体は `live_slots()` から外され、状態は `DTV_ENCLOSURE_DISCONNECTED` となって tombstone として保持される。
  - **回収境界:** 呼び出し境界（`refresh` や `next_event` の開始時）において、hold参照（`hold`/`drop`）、open参照（`retain_open`/`release_open`）、未配送イベント参照、および直前に配送したイベント（`last_delivered`）の参照がすべてゼロになった tombstone のみが回収（解放）される。
- **FIFOイベントジャーナルと保存不能時の挙動:**
  - イベント種別は正本の決定11に対応する6種（`DTV_EVENT_USB_CONNECTED`, `DTV_EVENT_USB_DISCONNECTED`, `DTV_EVENT_CARD_INSERTED`, `DTV_EVENT_CARD_REMOVED`, `DTV_EVENT_LOCK_ACQUIRED`, `DTV_EVENT_LOCK_LOST`）をサポートする。
  - USB差分プロデューサー: `refresh` 時に差分から `DTV_EVENT_USB_CONNECTED` / `DISCONNECTED` を自動生成してキューイングする。
  - CARDおよびLOCKイベント: `enqueue_event` 内部口を通じて投入可能（合成試験用。実デバイスやカードによる実プロデューサーは後続工程であり本PRでは未実装）。
  - キューは件数固定上限のないFIFO（`std::list`）。
  - **保存不能時の挙動（決定50）:** イベントメモリ確保失敗時（`prepare_events` 内の例外）は、**新走査のコミットは行わず、既にキューに保存済みのイベント列およびレジストリ状態をそのまま保持する**。保存済みイベントがすべて `next_event` で配送し尽くされた後、以降の呼び出しで sticky な内部 `RESOURCE_EXHAUSTED` を返す。セッションの強制終了（abort / exit）や既存参照の破棄は行わない。
- **走査失敗とジャーナル保存不能の違い:**
  - 走査失敗（USB列挙エラー、不正なグループ選択、重複接続クレームなど）: 新走査は破棄され、既存の live slots やイベントキューは変更されない（`invalid()` / エラーコード返却）。
  - ジャーナル保存不能: 新走査はコミットされないが、既存の保存済みイベントは保持・配送され、配送後に sticky `RESOURCE_EXHAUSTED` となる。

## 公開探索・寿命・イベント待機（C ABI）

PR25、PR26、PR27、PR28、PR29では、公開C ABI（`include/dtv/dtv.h` および生成ヘッダ `${PRODUCT_NAME}_api.h`）として以下のエントリポイントを内部レジストリへ接続している。

- **接続済み公開API一覧:**
  - 容量・共通列挙: `dtv_get_list_capacity`, `dtv_list_enclosures`, `dtv_get_scan_complete`
  - 製品別列挙（生成ラッパー）: `dtv_list_enclosures_<product>`（`${PRODUCT_NAME}_api.h`）
  - 未グループ化列挙: `dtv_list_ungrouped_devices`
  - tokenコールバック設定・選択列挙: `dtv_set_token_of`, `dtv_list_enclosures_by_tokens`, `dtv_list_enclosures_by_tokens_<product>`（`${PRODUCT_NAME}_api.h`）
  - FD選択列挙: `dtv_list_enclosures_by_fds`, `dtv_list_enclosures_by_fds_<product>`（`${PRODUCT_NAME}_api.h`）
  - 寿命管理: `dtv_hold_enclosure`, `dtv_drop_enclosure`
  - イベント待機: `dtv_wait_event`
  - 筐体getter: `dtv_get_enclosure_id`, `dtv_get_product`, `dtv_get_model`, `dtv_get_enclosure_serial`, `dtv_get_serial_unique`, `dtv_get_status`, `dtv_get_lnb_state`, `dtv_get_card_state`, `dtv_get_card_generation`, `dtv_get_card_reader_device`, `dtv_get_devices`, `dtv_get_candidates`
  - デバイスgetter: `dtv_get_device_id`, `dtv_get_vid`, `dtv_get_pid`, `dtv_get_device_serial`, `dtv_get_bus`, `dtv_get_address`, `dtv_get_port`, `dtv_get_mode`, `dtv_get_receivers`

- **公開列挙と走査特性:**
  - 共通一覧 `dtv_list_enclosures` に加え、CMake製品登録により生成される `<product>_api.h` から製品別一覧ラッパー `dtv_list_enclosures_<product>(dtv_error *err)` を提供する。
  - 呼び出し元スレッドで同期的にscan-only走査（ワーカー数0）を実行し、完全一致（`COMPLETE`）の筐体のみを固定容量配列で返す。
  - `dtv_get_scan_complete` は走査を行わず、レジストリの走査完了状態を返すだけのgetterである。
- **製品別一覧の投影特性と共有レジストリ:**
  - 製品別一覧は、Coreが所有するプロセス寿命の不動固定配列（容量は `dtv_get_list_capacity()` で取得）を返す。
  - 共通一覧と同じスロットインデックス（固定長・穴非圧縮）を持ち、当該製品に一致するスロットには共通一覧と同一の `dtv_enclosure*` が格納され、他製品のスロットまたは未割当スロットは `NULL` となる。
  - 製品ごとに別のレジストリ、走査、ワーカースレッド、イベントキューを生成せず、単一の探索セッション・レジストリを共有する。
  - 共通一覧、任意の製品別一覧、またはイベント待機（`dtv_wait_event`）の成功時に、走査結果に基づいて登録済みの全製品投影配列が一括同期（更新）される。
  - 配列のストレージはCoreがプロセス寿命で保持する不動領域（ヒープ上の固定配列）であり、後から別製品が追加されても既存配列のアドレスは移動しない。
  - 走査失敗やジャーナル確保失敗などのエラー時は `NULL` を返しエラーコードを設定する。部分的な投影（partial result）や空成功は公表せず、前回の成功状態の配列が維持される。
- **未グループ化デバイス一覧（`dtv_list_ungrouped_devices`）と不変要素:**
  - 既存宣言 `const dtv_ungrouped_device *const *dtv_list_ungrouped_devices(void)` は、Coreが所有する NULL 終端ポインタ配列を返す。
  - 正常空列時は `[NULL]`（先頭要素が NULL である非NULL配列ポインタ）を返し、走査・分類・メモリ確保の失敗時は `NULL` を返し `scan_complete` を 0 とする（エラー引数 `dtv_error*` は持たない）。
  - **候補抽出条件:** ロード済み製品の機種が宣言した VID:PID（公開 model の VID:PID および `ModelEntry.cold_identities`）に一致する接続のみが対象。未ロード製品や未宣言の VID:PID は除外される。
  - **完全グループ接続の除外:** `DTV_USB_GROUP_COMPLETE` と判定されたすべてのグループで使用された接続（スロット容量超過で live slots に載らなかった COMPLETE も含む）は除外される。
  - **1接続1要素と機種特定:** 同一の USB 接続に対して1要素のみ生成する。同一 VID:PID を複数機種が宣言していても重複生成しない。宣言機種が一意に定まる場合のみ `model` フィールドに機種識別子を設定し、複数機種に一致して特定できない場合は `NULL` とする。
  - **未観測値:** `serial` は常に `NULL`、`status` は常に `DTV_UNGROUPED_UNKNOWN` を返す。これらは現行走査では観測されないため、推測や架空の値を設定しない。
  - **不変要素・寿命と回収境界:** 同一接続かつ同一公開値であれば同一の要素ポインタを維持し、値が変化した場合は新しい不変要素を生成する。抜去・置換された旧要素は、次回の公開一覧またはイベント待機呼び出し（`dtv_list_enclosures`、`dtv_list_enclosures_<product>`、`dtv_list_ungrouped_devices`、`dtv_list_enclosures_by_tokens`、`dtv_list_enclosures_by_tokens_<product>`、`dtv_list_enclosures_by_fds`、`dtv_list_enclosures_by_fds_<product>`、`dtv_wait_event` の開始境界 `begin_public_boundary()`）まで保持される。
  - **hold APIなし・決定47直列化:** `dtv_ungrouped_device` に対する hold API（参照カウント）は存在しない。次回の境界を越えて値を使用する場合、呼び出し側は境界前に必要な値をコピーしなければならない。呼び出し側の配列参照は直列化区間で行う（決定47）。
  - **容量の独立性:** ungrouped の要素数には筐体スロット上限 `DTV_MAX_ENCLOSURES` は適用されない（`dtv_get_list_capacity()` は筐体スロットの固定容量を返す関数である）。
  - **イベント非発行:** 共通の探索セッション・レジストリを共有し、未グループ化接続の出入りだけで筐体イベント（`DTV_EVENT_USB_CONNECTED` / `DISCONNECTED`）は発行されない。
- **tokenコールバック設定とtoken選択一覧（`dtv_set_token_of` / `by_tokens`）:**
  - **コールバック設定:** `void dtv_set_token_of(dtv_token_of_callback cb, void *user)` は、探索（`list` / `wait`）の初回呼び出し前に一度だけ設定する。コールバック関数ポインタおよび `user` ポインタは呼び出し側（caller）がプロセス寿命で所有する。二重設定や探索開始後の再設定は拒否される。
  - **同期実行契機:** 共通の `dtv_list_enclosures_by_tokens` および製品別の `dtv_list_enclosures_by_tokens_<product>` の呼び出し中、呼び出し側スレッドによる同期USB走査（`scan_only` 列挙中）においてのみ、借用した生 `libusb_device` を引数として `cb(user, dev)` が同期実行される（コールバック中からの L3 API 再入は不可）。通常の共通一覧・製品別一覧、未グループ化一覧、およびイベント待機では呼び出されない。
  - **外部tokenと内部接続epochの分離:** 外部token（`dtv_usb_token`）の発行・失効は呼び出し側（WebUSB層等）が管理し、0 は選択外を表す。外部tokenはCore内部の接続epoch（`connection_token`）とは別管理される。
  - **選択投影と全観測レジストリの分離:**
    - 要求された `tokens` 配列の順序から USB function や筐体グループの順序・根拠を推測しない（既存の機種グループ判定規則に従う）。
    - 走査では接続中の全デバイスを列挙して共有レジストリの生存状態を更新し、選択されなかった物理接続が存在しても共有レジストリ上で `DISCONNECTED` への誤切断や偽の切断イベント発行は行われない。
    - 選択された完全グループのみが固定容量配列（`dtv_get_list_capacity()` 個の枠）の token 選択 view へ投影される。同一の物理接続に対応する root は、通常一覧、製品別一覧、token 選択一覧のすべてで同一の `dtv_enclosure*` ポインタを保持する（片側選択時は空スロット、空選択時はすべて NULL の固定配列を正常返却）。
  - **エラー境界と入力検証:**
    - コールバック未設定での呼び出し: `DTV_ERROR_INVALID_ARGUMENT`
    - `tokens == NULL && token_count > 0` または `tokens != NULL && token_count == 0`: `DTV_ERROR_INVALID_ARGUMENT`
    - 要求配列に token 0 が含まれる、または重複 token が含まれる場合: `DTV_ERROR_INVALID_ARGUMENT`
    - コールバックが異なる複数の接続に対して同一の非ゼロ token を返した場合（重複コールバック）: `DTV_ERROR_INVALID_ARGUMENT`（診断メッセージ付き）
    - 要求された token が現在の走査に存在しない場合（不在または失効した token）: `DTV_ERROR_NOT_FOUND`
    - エラー情報は `dtv_error` の `struct_size` およびメッセージバッファ容量内で安全に書き込まれる。
    - 走査失敗、入力検証失敗、メモリ確保失敗等のエラー時は `NULL` を返し、部分投影（partial result）や中途半端な更新は公表しない。
  - **寿命と決定47の直列化規則:**
    - token 選択配列自体はCore所有の不動固定配列であり、root を独自 hold しない。
    - 呼び出し側は一覧取得・配列参照から `dtv_hold_enclosure` による hold までを同一スレッド上で行うか、呼び出し側のロックで直列化する責任を負う（決定47）。
    - 次回の公開境界（次回の通常 list / 製品 list / ungrouped list / token list / fd list / wait_event 呼び出し）で既存の token 選択投影は更新・回収される。
- **FD選択一覧（`dtv_list_enclosures_by_fds`）と所有権・OS境界:**
  - **署名と生成ヘッダ:** 共通の `dtv_enclosure *const *dtv_list_enclosures_by_fds(const int32_t *fds, uint32_t fd_count, dtv_error *err)` および製品別の `dtv_list_enclosures_by_fds_<product>(const int32_t *fds, uint32_t fd_count, dtv_error *err)`（`${PRODUCT_NAME}_api.h`）。
  - **対応OSと非対応OS:** Linux および Android (Linux ABI) に対応。非Linux（Windows等）では `DTV_ERROR_UNSUPPORTED` を返し、`NULL` を返す。
  - **FDの所有権とLease管理:**
    - 呼び出し側から渡された入力FD（borrowed FDs）は変更せず、`close()` も行わない。
    - 各入力FDを `fcntl(fd, F_DUPFD_CLOEXEC, 0)` で複製し、複製FDを `libusb_wrap_sys_device` へ渡す。
    - `UsbHandleLease` が資源を管理し、`wrap handle` を先に close（`libusb_close`）し、その後に `複製FD` を close（`close(fd)`）する。
    - 成功時: snapshot と root がこの lease を共有所有し、同じ root の再投影で置換された時、または root が回収された時に解放される。
    - 失敗時（検証・wrap・descriptor取得・分類・確保失敗）: 未公開 snapshot と複製資源を一括破棄・解放する。
    - **注意:** 公開 enclosure open / claim / transfer worker の完成と混同しない（wrap handle 自体は内部に存在するが、公開 open API の完了ではない）。
  - **グループ判定と全観測・選択投影の挙動:**
    - 要求FD配列の順序から USB function を推測せず、既存セレクタの完全グループ（`DTV_USB_GROUP_COMPLETE`）のみを判定する。
    - 渡された FD に対応する完全グループのみを固定容量配列（`dtv_get_list_capacity()` 枠）へ投影する。
    - FD一覧に含まれない既存 root や ungrouped view を勝手に切断（DISCONNECTED）したり破棄したりしない。
    - 同一物理 port で connection epoch が変わった完全グループ（抜去・再挿入など）は古い root を切断し、新しい root として扱う。
    - 通常一覧、製品別一覧、token選択一覧、FD選択一覧で、同一の物理接続・完全グループに対応する root は同一の `dtv_enclosure*` ポインタを保持する（片側選択時は空スロット、空選択時はすべて NULL の固定配列を正常返却）。
  - **再列挙と供給API:**
    - 一覧取得経路は open / claim を行わず、worker スレッドを開始しない。
    - 再列挙後に新しい FD が必要になった場合は、呼び出し側がパーミッションを再取得して一覧APIを呼び直す（FD を追加供給する supply API は追加しない）。
  - **入力検証とエラー境界:**
    - NULL/count不整合、負値のFD、無効/閉じたFD、重複FD、同一接続を指す複数FDは `DTV_ERROR_INVALID_ARGUMENT`。
    - libusb によるアクセス拒否（権限不足等）は `DTV_ERROR_PERMISSION_DENIED`。
    - 非Linuxは `DTV_ERROR_UNSUPPORTED`。
    - エラー情報は `dtv_error` の容量内で安全に書き込まれ、失敗時は `NULL` を返して部分投影は公表しない。
  - **寿命と決定47の直列化規則:**
    - FD選択配列自体は Core 所有の不動固定配列であり、root を独自 hold しない。
    - 呼び出し側は一覧取得・配列参照から `dtv_hold_enclosure` による hold までを同一スレッド上で行うか、呼び出し側のロックで直列化する責任を負う（決定47）。
    - 次回の公開境界（次回の通常 list / 製品 list / ungrouped list / token list / fd list / wait_event 呼び出し）で既存の FD 選択投影は更新・回収される。
- **配列・文字列所有と決定47の直列化規則:**
  - `dtv_get_devices` および `dtv_get_candidates` が返す配列は NULL 終端であり、root（`dtv_enclosure`）が所有する。
  - 製品名、モデル名、および位置パス文字列（"BUS-port[.port...]" 形式、未観測時はNULL）も root が所有する。
  - **重要（決定47）:** 共通一覧配列、製品別一覧配列、token 選択一覧配列、および FD 選択一覧配列自体は root を独自に hold しない（root の寿命はレジストリが所有する）。したがって、呼び出し側は、`dtv_list_enclosures`、`dtv_list_enclosures_<product>`、`dtv_list_enclosures_by_tokens`、`dtv_list_enclosures_by_tokens_<product>`、`dtv_list_enclosures_by_fds`、または `dtv_list_enclosures_by_fds_<product>` による一覧取得から配列参照、および対象筐体の `dtv_hold_enclosure` による hold までを、同一スレッド上で行うか呼び出し側のロックで直列化しなければならない。内部の API mutex はCore内部のセッション・レジストリ境界を保護するものであり、呼び出し側の配列読み取り〜hold の直列化を代行するものではない。hold されていないポインタは、次回の `dtv_list_enclosures`、`dtv_list_enclosures_<product>`、`dtv_list_ungrouped_devices`、`dtv_list_enclosures_by_tokens`、`dtv_list_enclosures_by_tokens_<product>`、`dtv_list_enclosures_by_fds`、`dtv_list_enclosures_by_fds_<product>`、または `dtv_wait_event` 呼び出し（回収境界）までしか安全に参照できない。
- **観測値と未観測値の区別:**
  - 観測値: VID, PID, bus, address, port。
  - 未観測値（初期値）: serial は NULL、uniqueness は `DTV_UNKNOWN`、card / LNB / mode は `UNKNOWN` 値、card generation は 0、card-reader および receiver 一覧は NULL を返す。架空の値や機種情報を創作しない。
- **イベント待機（`dtv_wait_event`）:**
  - 保存済みイベントを FIFO 順で1件ずつ配送する。
  - 直前に配送したイベント（`last_delivered`）の対象オブジェクトは、次回の境界（次回の `dtv_list_enclosures`、`dtv_list_enclosures_<product>`、`dtv_list_ungrouped_devices`、`dtv_list_enclosures_by_tokens`、`dtv_list_enclosures_by_tokens_<product>`、`dtv_list_enclosures_by_fds`、`dtv_list_enclosures_by_fds_<product>`、または `dtv_wait_event`）まで参照が保持される。
  - `timeout_ms == 0` は無期限待機、タイムアウト時は公開 `DTV_ERROR_TIMEOUT` を返す。
  - 保存済み FIFO イベントをすべて配送し切った後、ジャーナル保存不能フラグが立っていれば sticky な公開 `DTV_ERROR_RESOURCE_LIMIT` を返す（決定50。内部 `RESOURCE_EXHAUSTED` からの写像）。OOM 発生時もセッションの強制終了や既存参照の破棄は行わない。
  - **キャンセル入力なし:** 現行の `dtv_wait_event` 署名（`(dtv_event *, const dtv_call_options *, dtv_error *)`）にはキャンセル入力は存在せず、未存在のキャンセル API は提供しない。

## 完全筐体の私有claim取得とLease所有（内部基盤）

PR30では、後続の公開openや1-workerランタイムへ物理claimを引き渡すための内部基盤（`EnclosureRegistry::claim_inputs` および `UsbContext::acquire_claims`）を追加している。これは内部privateな処理であり、公開C ABIや製品SPIの追加ではない。また、公開 `dtv_open_enclosure` などの公開open完了ではない。

- **rootからの入力生成（`EnclosureRegistry::claim_inputs`）:**
  - 呼び出し側が `dtv_hold_enclosure` で保持した、`READY` 状態の完全rootをmutex保護下で確認する。
  - rootが所有する `UsbDeviceSnapshot` および `SelectedFunction` から、同一の物理接続（`device->connection` = 物理接続epoch）ごとに `UsbClaimDeviceInput`（`connection_epoch`, `snapshot`, `interfaces`）へ束ねる。同一物理デバイス上の複数functionは単一の物理入力に統合される。
  - 選択interfaceの一意性、active configurationとの整合、物理接続epoch集合の完全一致を検証する。
  - rootポインタ、固定一覧view、イベントキューの状態は変更しない。
- **事前検証とLease所有（`UsbContext::acquire_claims`）:**
  - 呼び出しスレッド上で同期実行される（スレッド作成なし、worker数0）。
  - **事前検証:** input epochとsnapshot token/context、重複physical epoch/interface、active configurationのinterface/alternateをclaim前に検証する。backendがidentityを提供する環境では、snapshot取得時のUSBFS identityと現在値を照合し、古いsnapshotをNO_DEVICE detail付きUSB_IOとして拒否する。
  - **所有形態とLease管理（`UsbClaimHandleLease` / `UsbClaimLeaseGroup`）:**
    - 通常USB: 物理epochごとに `libusb_open` したhandleを `UsbClaimHandleLease` が所有（`owns_handle = true`）し、必要interfaceをclaimする。
    - FD由来: snapshotが保持する既存の `UsbHandleLease`（wrap handle）を借用（`wrapped_owner`、`owns_handle = false`）し、claimのみを追跡する。呼び出し側の元FDはdup・close・変更しない。
  - **逆順rollbackと即時claim解除:**
    - 途中失敗時または明示的な `release()` 時は、当該handle内の claimed interfaces を逆順に `release_interface` し、device順を逆に戻して先行deviceの interfaces も逆順releaseする（通常handleは `close_handle`）。
    - FD由来では snapshot が存続していても claim だけは直ちに解除され、wrap handle 自体は snapshot の所有規則へ戻る。
  - **期限・キャンセル・エラー境界:**
    - 単調時計の `absolute deadline`（`deadline_ns`）および `dtv_cancel`（`is_cancelled`）を各同期USB段（open, claim, set_alt）の前後のチェックポイント（`claim_checkpoint`）で確認する。期限切れやキャンセル時は `DTV_INTERNAL_RESULT_TIMEOUT` を返す。同期libusb呼出し自体の途中中断は保証しない。
    - claimやset_alt時の `LIBUSB_ERROR_BUSY` は `DTV_INTERNAL_RESULT_BUSY` とlibusb detailを保持する。
- **後続工程との境界:**
  - 1筐体1workerのランタイム（`EnclosureRuntime`）および公開open/close/state/release/cancel実資源統合はPR31/PR32で接続された。製品固有identifyや製品エンドポイント設定、TS/FW/daemonは後続工程。

## 筐体単位の非同期1-workerランタイムと停止回収（内部基盤）

PR31では、PR30で取得した完全筐体の実claim lease（`UsbClaimLeaseGroup`）をmove所有し、同一context下の複数物理handleに対する非同期USB転送を駆動・管理する私有ランタイム（`EnclosureRuntime`）を追加している。これは内部privateな処理であり、公開C ABIや製品SPIの追加ではない。また、公開 `dtv_open_enclosure` などの公開open/close完了や製品固有処理ではない。

- **1-workerスレッドモデル（`EnclosureRuntime::start` / `worker_count`）:**
  - `EnclosureRuntime::start` は、取得済みかつ空でない `UsbClaimLeaseGroup` をmoveで受け取り、単一のscan-only `UsbContext` 状態を共有する全handleを管理する。
  - 筐体ごとにちょうど1本の専用イベントポンプスレッド（worker数1）を生成し、イベント駆動（`libusb_handle_events` 等）、完了ディスパッチ、キャンセル/reap、期限wakeを進める。
  - 未openのscan/FD/token一覧およびclaim取得時はworker数0のまま維持される。
- **非同期リクエストと資源管理（`submit_bulk_out` / `submit_bulk_in`）:**
  - Bulk IN/OUTリクエストは呼び出し側バッファを内部 `std::vector<unsigned char>` にコピーして自己所有する。
  - 各リクエストには固有の `request_id`、単調増加の `submit_sequence`、および投入時の `generation` が割り振られる。
  - **内部64枠の資源制約:** 内部で同時に追跡可能なリクエスト数は最大64件（`kRequestCapacity = 64`）に制限され、超過時や停止中は即時拒否される（`DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED` / `NOT_READY`）。これは内部リソース上限であり、公開仕様やスループット/タイミングのSLAに昇格しない。
- **完了処理と短い受け口（`UsbRuntimeCompletion` / `next_completion`）:**
  - workerスレッドが短い間隔でイベントを処理し、コールバックを回収（reap）。deadline scan/cancel dispatch/reap は固定64枠の配列を用いて処理され、worker ループ内での一時 heap 確保を行わない。
  - 実転送長（`actual_length`）および終端状態（`completed`, `cancelled`, `deadline`, `endpoint_cancelled`, `stopped`, `failed`）を記録。
  - コールバック回収順に単調増加の `completion_sequence` でキューイングされる。なお、`wait_request` による waiter への terminal 通知と completion queue への要素発行は別段階で進められる。
  - **遅延完了とリクエストの結び付き:** 転送ごとの fresh な request オブジェクトと `transfer->user_data` によりコールバックが元の request へ一意に結び付く所有形態をとる。実装に明示的な generation や request ID の等値照合・比較チェックは存在しないが、コールバック返却時に元 request の `generation` や `request_id` が completion メタデータへそのまま保持され、遅延コールバックが別の新しいリクエストへ誤って再バインド・配送されることはない。
  - INデータは成功完了（`completed` かつ結果OK）時のみ `completion.bytes` へ移動され、失敗やキャンセル時はバッファが破棄される。
- **キャンセル・期限・同エンドポイントピア取消:**
  - `cancel` は呼び出し側へ速やかに復帰し、workerがlibusbのキャンセルを発行。
  - バックエンドから終端コールバックが返るまで、リクエストの内部バッファや転送管理領域は解放されない。
  - 同一エンドポイント上の待機リクエストで自身のキャンセル理由がないものは、`endpoint_cancelled` として報告される。
- **停止処理と所有権回収（`stop` / `join_release`）:**
  - `stop` 要求を受けると新規submitを即時拒否し、保留中のリクエストに `stopped` をマークする。
  - **私有pending:** 指定されたstop期限内にコールバックがdrainし切らない場合、`stop` は `pending = true` を返し、転送オブジェクト、バッファ、FD wrap lease、claimを保持し続ける。
  - **workerスレッドの非同期責務:** workerスレッド自身はlibusbイベント処理とコールバック回収に専念し、自己デッドロックを防ぐため、同期USB I/O、製品closeコールバック、ブロッキングなcleanup、最終的なclaim解放やhandle closeをworkerスレッド上で行わない。
  - 遅延コールバックがすべて返却されてからworkerスレッドが退出する。
  - 呼び出し側スレッド（caller thread）で `join_release()` がworkerをjoinし、`UsbClaimLeaseGroup::release()` によりclaimとhandleを逆順解放する。
- **後続工程との境界・未解決境界:**
  - 公開open/close/PENDING/CLOSED統合、初回共有IN送信、および同一FD snapshot・物理identityに対するlive claim排他予約はPR32で接続された。実機5製品の固有identify/共有IN demux、製品endpoint設定、TS/FW/daemonは後続工程。

## 合成機種による公開open/close実資源統合と排他制御（公開L3・内部基盤）

PR32では、完全筐体rootを公開C ABI（`dtv_open_enclosure`, `dtv_close_enclosure`, `dtv_get_enclosure_close_state`, `dtv_release_enclosure`, `dtv_cancel_enclosure`）へ接続し、実claim lease、1-workerランタイム、初回共有IN先行受信、および同一資源の排他予約（claim reservation）を統合している。

- **公開openシーケンス（`dtv_open_enclosure`）:**
  - 呼び出し側が保持（hold）した完全root（`dtv_enclosure`）から、`EnclosureRegistry::open_description` によりモデル情報、物理接続epochごとの入力（`UsbClaimDeviceInput`）、機能一覧を取得する。
  - **私有open計画（`PublicOpenOps`）:** `ModelEntry::public_open` は合成機種または対応モデルがopt-inで提供する私有構造体（`plan`, `identify`, `stop` hook）であり、Core内部に実機固有の仮値を創作しない。
  - **事前検証:** 未対応モデル（`public_open == nullptr`）、不正plan（存在しないinterface/endpoint、IN方向以外の指定、非bulk転送、バッファ不整合等）、抜線・古い接続epochの場合はUSB I/O前に拒否する（`DTV_ERROR_UNSUPPORTED` / `DTV_ERROR_INVALID_ARGUMENT` 等）。ただし `open_slots` に失敗用slotを割り当て、後続の `close` / `state` / `release` で安全に回収できる。
  - **排他制御（live claim予約）:**
    - 同一rootの二重openは即時 `DTV_ERROR_BUSY`。
    - 物理connection identity / epoch、および FD wrap handle 所有者（`wrapped_owner`）に基づく `ClaimReservation` を `UsbContext::acquire_claims` 前に確保。通常/FD投影や同じFD snapshot/wrap handleを跨ぐ重複live claimはclaim前に `DTV_ERROR_BUSY` で拒否する。rollback終了または `CLOSED` まで排他予約を維持する。
    - 異なる physical root 間では共通mutexをUSB/identify完了まで保持せず、別workerで並行進行できる。
    - 呼び出し側の入力FDはborrow扱いで維持し、dupやcloseは行わない。
  - **実claim・1-worker・初回共有IN・identify:**
    - 全物理handleのclaim leaseを取得する（未open時はworker 0）。
    - 筐体ごとにちょうど1本の専用workerスレッドを持つ `EnclosureRuntime` を開始する。
    - planで指定された全共有INリクエストの初回submitを行い、worker上で先行受信を開始する。
    - 実callback/completionの受信 payload を用いて、callerスレッド上で `identify` を同期実行する（identify待ちはcaller、イベント処理とreapはworker）。
    - 全段階を通過して初めて公開 `DTV_ERROR_OK` を返す。
- **失敗時の資源回収:**
  - claim途中、worker開始、初回IN submit、identifyの各失敗時、先行資源を逆順回収する。
  - open途中失敗後も、`dtv_close_enclosure` → `dtv_get_enclosure_close_state` → `dtv_release_enclosure` の共通ライフサイクルで安全に回収できる。
- **公開close・cancel・PENDING/CLOSED状態遷移:**
  - `dtv_cancel_enclosure`: 別のthreadから呼び出し可能。atomic flag（`slot->cancelled`）をセットし、runtimeの進行中リクエストをキャンセルしてwakeする。
  - `dtv_close_enclosure`:
    - 外部threadからgateを閉じ、製品 `stop` hookをcallerスレッドで実行後、runtimeをdrainする。
    - 単調時計の `absolute deadline`（`deadline_after`）を全段で適用する。`timeout_ms == 0` は無期限待機、`options == nullptr` は既定タイムアウト。同期libusb呼出し自体の途中中断は保証しない。
    - 期限内にdrain/joinできれば `DTV_CLOSE_CLOSED` を返す。
    - 期限切れで未完了の場合は `DTV_CLOSE_PENDING` を返し、runtime、transfer buffer、lease、FD wrap owner、root、暗黙hold（implicit hold）をすべて維持する。
  - **PENDING中とCLOSED後の動作:**
    - PENDING中の `dtv_release_enclosure` は no-op（資源を破棄しない）。
    - `dtv_get_enclosure_close_state` により、遅延callback返却後にworkerが退出したことを検知し、caller側で `join_release` して `DTV_CLOSE_CLOSED` へ遷移する。
    - CLOSEDになった後、`dtv_release_enclosure` を呼ぶことでslot、live claim予約、暗黙hold、root資源を最終解放する。

## 継続共有IN ownerと有界返信配送（内部基盤）

PR33では、PR32で導入された公開openの初回IN受信に続き、筐体に属する同一の1-workerランタイム（`EnclosureRuntime`）が共有INを自動で継続投入（re-submit）し、受信パケットを短い同期demuxによって待機者へ有界配送する私有基盤（`SharedInOwner`）を追加している。

- **初回から継続への所有維持と自動再投入:**
  - 公開openの初回IN受信を `identify` / `identify_shared` に渡しつつ、同一の筐体workerスレッド上で共有INコールバックを連続して再投入する。
  - 各planの `shared_in_depth` に応じて同一endpointの複数requestを実submitし、展開後の全request数は内部上限64件に制限される。
  - **同一endpoint内の順序保証:** 同一handle/endpointにおいてcallbackが逆順に到着した場合、先行submit sequenceのcallbackがretireされるまで後続callbackのretireおよびdemux配送を保留する。異なるendpoint間は独立して並行処理される。
  - 呼び出し側による完了ポーリング（`next_completion`）を不要とし、open復帰後も受信が途切れない。複数handleの共有INも単一のworkerスレッドで処理される。
  - 各完了ごとにtransferをretire / freeし、単調増加する新しい `submit_sequence` を割り当てる。
  - 連続完了をone-shotの完了キュー（64枠）に蓄積せず即時回収・再投入するため、完了枠の枯渇を起こさない。
  - 共有INバッファは自己所有され、実コールバックの `actual_length`、endpoint、sequence、generationを保持して短いdemuxへ渡す。
- **私有opt-in計画と短いdemux・有界返信配送:**
  - モデルは私有の `PublicOpenPlan`（`shared_in_depth`, `prestream_sink_limit`, `reply_capacity`）および同期 `demux` をopt-inで供給する。
  - `PublicOpenOps::demux` はworkerスレッド上で受信パケット（`PublicOpenPacket`）を短時間で同期分類し、`PublicOpenMessage`（`ignore`, `notification`, `control_reply`, `source_chunk`）を返す。
  - 返信（`control_reply`）は、`(resource_scope, transaction_id, connection_generation)` の組で照合され、該当する待機者の固定mailboxへ配送される。
  - **キャンセル・期限切れとtombstone:** 操作waiterのキャンセル（`cancel_waiter`）は、未終端waiterへ `cancelled` 結果と `ready = true` を設定して即時起床通知を行う（完了済みwaiterの終端結果は上書きしない）。期限切れ・取消済みの `(scope, transaction, generation)` はtombstone（最大16件）として記録され、遅れて戻ってきた返信を吸収して同一世代の別の後続waiterへ誤配送されるのを防ぐ。固定16件のtombstone枠が枯渇した場合は、不整合な継続を防ぐため `RESOURCE_EXHAUSTED` terminalとしてownerを停止し、明示的エラーを通知する。操作キャンセルによって共有IN subscription全体がキャンセルされることはない。
  - **固定容量制約（内部上限、公開SLA非昇格）:**
    - 返信mailbox: 最大16件（`replies`）
    - 返信waiter: 最大16件（`waiters`）
    - late reply tombstone: 最大16件（`tombstones`）
    - prestream sink: 計画値合計かつ最大1 MiB
    - 共有INバッファ・reply capacity: 1件最大1 MiB、plan/request枠は最大64件
    - これらは内部資源上限であり、公開仕様やスループット/タイミングのSLAに昇格しない。容量超過時は `RESOURCE_EXHAUSTED` をwaiterへ通知する。
- **安全停止とエラー・OOM隔離:**
  - 通常の操作キャンセルでは共有IN subscriptionを停止しない。
  - 筐体close（`dtv_close_enclosure`）時にownerをfenceして待機中のwaiterを起こし、既存のruntime stop/cancel/reap/join経路へ接続する。
  - 遅延callbackが残る場合は、PENDING中のslotがowner、buffer、`ProductInstance`、runtime、claim、rootをすべて保持する。
  - 分類処理での `std::bad_alloc`、sink overflow、再submit失敗（`LIBUSB_ERROR_IO` 等）は、理由付きterminalとしてownerへ通知され、workerの `noexcept` 境界から例外を漏らさず、プロセスを無言停止・abortさせない。
  - **決定50との区別:** 今回の内部mailbox / sinkの有界性・超過通知は、公開event FIFOの「保存件数は無上限、OOMならsticky RESOURCE_LIMIT（決定50）」とは別レイヤの内部機構であり、混同しない。
- **後続工程との境界:**
  - 公開tune / card / stream操作、CARD / LOCK公開イベントproducer、TS framer / transform、各操作ticket、実機5製品の初期化・パラメータ・分類は後続工程。合成機種における継続共有IN受信および返信配送の完成と区別する。

## 共通操作受付と物理scope ticket（内部基盤）

PR34では、開いた筐体に対してcaller側から投入される操作の受付判定、短いgateによる物理資源予約（`OperationTicket`）、非同期OUT送信と共有IN返信の連携、およびキャンセル・停止時の安全な回収を行う私有基盤を追加している。

- **共通操作受付と事前検証（`PublicOpenOps::plan_operation` / `PublicOperationPlan`）:**
  - 呼び出し側スレッド（caller thread）から共通の操作受付関数（テスト用 `run_public_operation_for_test` 等）を通じて、私有の `PublicOperationRequest`（`operation`, `target_kind`, `target_id`, `arguments`）を受け付ける。
  - **正本§6.1の優先順位を遵守した事前判定:**
    - 機器切断状態（`DISCONNECTED`）→ `DTV_INTERNAL_RESULT_DISCONNECTED`
    - 宣言非対応 / plan未提供（`ops->plan_operation == nullptr` または `!plan.supported`）→ `DTV_INTERNAL_RESULT_UNSUPPORTED`
    - 機種未確定 / firmware未準備（`!plan.ready` または slot phase `!= open`）→ `DTV_INTERNAL_RESULT_NOT_READY`
    - 引数不正（`!plan.arguments_valid`、target_kind 不正、または plan の resource keys が最大8個を超過等）→ `DTV_INTERNAL_RESULT_INVALID_ARGUMENT`
    - これらはいずれも **USB I/O を発行する前に拒否** される。
- **短いgateでのticket予約と物理scope排他（`OpenSlot::OperationTicket` / `acquire_operation_ticket`）:**
  - `plan_operation` から得られた target（`target_kind`, `target_id`）および物理資源キー（`resource_keys`、最大8キー）を、短いmutex保護下のgateでアトミックに予約する。
  - **同一scope直列化と独立scope並行:** 同一のtargetまたは同一の物理resource key（例: 同一bridge / I2C / 電源線など）を要求する操作は先行操作の完了まで待機（直列化）。一方、異なるtargetかつ独立したresource keyを持つ操作は並行実行を許す。
  - **USB待ち中mutex非保持:** チケット確保後、受付gateのmutexは直ちに解放され、以降の非同期OUT送信・共有IN返信待ちの間はmutexを保持しない（デッドロック防止と並行性の確保）。
  - **呼び出し側直列化前提（正本§9・決定47）:** 同一receiver / stream操作および筐体全体操作はcaller側で直列化する義務があり、本機構は共有USB/物理資源の競合を内部で調停するものである。
  - **内部上限（公開SLA非昇格）:** 1オープン筐体あたりの同時 live operation ticket 枠は最大16件、plan が指定可能な物理 resource key は最大8個。plan の resource key 数超過（9個以上等）は事前判定で `INVALID_ARGUMENT` となり、16件の ticket 枠枯渇時は `RESOURCE_EXHAUSTED` を返す（いずれも OUT 未送信）。
- **OUT送信と共有IN返信の連携シーケンス:**
  - 非同期OUT転送を投入する前に、`SharedInOwner` へ返信待機者（waiter）を登録（`register_reply`）。
  - `EnclosureRuntime::submit_bulk_out` により非同期OUT転送を発行。
  - `EnclosureRuntime::take_completion` によりOUT転送の完了およびbackend到達の有無（`reached_backend`）を回収。
  - `SharedInOwner::wait_reply` により、共有INから `(resource_scope, transaction_id, connection_generation)` に合致する返信パケットをcallerスレッドで待機・取得する。
- **キャンセル・タイムアウトとチケットの安全な回収（`abandon_waiter` と retiring ticket）:**
  - 操作キャンセル（`cancel`）や期限切れ（`deadline`）時、callerスレッドは `CANCELLED` や `TIMEOUT` で即時起床する。
  - **retiring ticket による物理予約の維持:** callerスレッドが起床しても、送信済みOUT転送のコールバックが未完了の間は物理チケット（予約）を解放せず、`retiring` 状態として保持する（ハードウェアへの多重アクセス防止）。
  - **返信waiterとtombstoneの扱い:**
    - OUT転送がbackendへ到達していない（未到達）ことが確認された場合、`abandon_waiter` によりwaiterおよびtombstoneを直ちに破棄・再利用可能にする。
    - backendへ到達した可能性がある場合は、tombstoneを保持して遅れて届いた返信を吸収し、後続ticketへの誤配送を防ぐ。
  - 操作キャンセルによって共有IN subscription自体は停止しない。
  - 筐体close（`dtv_close_enclosure`）時は全操作をキャンセルし、遅延callbackがあればPENDINGとして資源・所有を保持し、callback drain後にCLOSED・回収する。
- **後続工程との境界:**
  - 本PRは共通操作受付・物理scope ticket・返信回収の私有基盤であり、公開receiver / card / tune / stream操作API（`dtv_tune`, `dtv_transmit_apdu`, `dtv_read_stream` 等）や実機5製品の実機コマンド・制御の実装は後続工程。

## 公開CARD状態とイベント配送（合成共有IN接続）

PR35では、開いた筐体の継続共有INから得られるCARD状態変化通知を、公開L3状態（getter）および公開イベントFIFO（`dtv_wait_event`）へ接続する機構を追加している。

- **カードリーダーのバインドと所有権（`EnclosureRegistry::bind_card_reader` / `PublicOpenPlan::card_reader_source`）:**
  - open計画（`PublicOpenPlan`）において `card_reader_source = true` を指定したhandle（同一オープン筐体内で高々1つ、未指定または重複指定時は `INVALID_ARGUMENT`）をカードリーダーの入力源として特定。
  - `bind_card_reader` により、当該入力の `connection_epoch` に合致する既存の公開 `dtv_device`（同一root内のデバイスオブジェクト）を `card_reader_device` としてバインド。
  - 公開getter `dtv_get_card_reader_device` はこのデバイスポインタを返し、非カード機種やopen前・未確定時は `NULL` を返す。
- **共有IN demuxからの状態通知と検証（`PublicOpenNotificationKind::card_state` / `EnclosureRegistry::observe_card_state`）:**
  - 継続共有INのworker上で、`open_ops->demux` が `PublicOpenMessageKind::notification` かつ `notification_kind == PublicOpenNotificationKind::card_state` を返したとき、通知された `card_state`（`ABSENT`, `PRESENT`, `READY`）を処理。
  - **厳格な検証と不適合通知の破棄:**
    - 筐体状態がREADYでない（切断時等）→ `DISCONNECTED`（共有IN処理はOKで継続）
    - 宣言外の不正値（`card_state > DTV_CARD_READY`）、未バインド、接続世代の不一致（旧世代）、またはカードリーダー以外のhandleからの誤source通知 → `INVALID_ARGUMENT`（破棄し共有INは継続）
    - 同一状態の再通知（状態変化なし）→ イベント発行なし・generation不変で正常復帰
- **公開状態更新とイベントFIFOへの接続（`dtv_get_card_state`, `dtv_get_card_generation`, `dtv_wait_event`）:**
  - 状態変化時に `root->card_state` を更新。
  - **イベント生成とgenerationの歩進:**
    - 非挿入（`ABSENT`）から挿入（`PRESENT` または `READY`）への変化で `DTV_EVENT_CARD_INSERTED` を発行。
    - 挿入状態（`PRESENT` または `READY`）から非挿入（`ABSENT`）への変化で `DTV_EVENT_CARD_REMOVED` を発行。
    - これらのエッジ変化時に限り `root->card_generation` をインクリメントし、順序付き公開イベントFIFO（`state_->events`）へ追加。
    - 生成される `dtv_event` の `enclosure` は当該root筐体を指し、`receiver` は `NULL`（正本§7・要件F-16遵守）。
  - **カード初回観測時の振る舞い（正本L3決定51）:**
    - 正本L3決定51（`spec/dtv11-l3-spec.md` §4.4/§7、`l3/dtv.h`、`l3/decisions.md`）に基づき、カードの状態が `DTV_CARD_UNKNOWN`（`generation = 0`）から初めて既知状態（`ABSENT`・`PRESENT`・`READY` のいずれでも）として判明した際は、開いたときの最初の状態が確認されただけとみなし、`CARD_INSERTED` / `CARD_REMOVED` イベントは発行せず、`dtv_get_card_generation()` は 0 のまま維持する。
    - 開いたときに既に挿入されていたカードは、`dtv_get_card_state()` が `PRESENT` または `READY` を返すことで確認できる。開いた後に実際に観測された抜き差しからイベントを発行し、generation を進める。
- **同期境界・参照寿命と決定50（OOM時のsticky RESOURCE_LIMIT）:**
  - **短いmutex保護とcaller直列化:** `SharedInOwner` は短いmutexでstoppingを確認し、demuxおよびUSB I/O待機中にはregistry mutexを保持しない。呼び出し側は決定47に基づき `dtv_wait_event`、`dtv_list_enclosures`、固定配列参照を直列化する義務がある。
  - **キャンセル・close時の振る舞い:** 通常の `cancel_enclosure` は実行中操作のみを停止し、共有IN subscriptionおよびCARD通知購読は継続する。`dtv_close_enclosure` 時は新規通知を遮断し、遅延callback完了（drain）までPENDINGとしてroot、reader device、event参照を保持し、CLOSED後に解放する。
  - **決定50（公開イベントjournal OOM時のsticky RESOURCE_LIMIT）:** イベント追加時のメモリ確保失敗（`bad_alloc`）時は、最新の観測状態（`card_state` / `card_generation`）は保持しつつ、既保存イベントの配送完了後に `dtv_wait_event` が同一セッションで `DTV_ERROR_RESOURCE_LIMIT` を返し続ける（未保存履歴の復元や通知合体・固定枠制限は行わない）。
- **後続工程との境界:**
  - 本PRは合成機種における共有INからのCARD状態・イベント接続であり、実機5製品のカード通知値・パラメータ、カードAPDU送受信（`dtv_transmit_apdu`）、ATR取得（`dtv_reset_card` / `dtv_get_card_atr`）、receiver / LOCK通知、選局・ストリーム（TS）機能の実装は後続工程。

## 安定receiver公開対象と宣言確定（機種宣言・動的確定接続）

PR36では、機種宣言に基づく受信部メタデータを安定した公開 `dtv_receiver` オブジェクト群へ実接続し、公開受信部getter・support判定、および未知状態から既知状態へのアトミックな宣言確定基盤を追加している。

- **機種宣言構造（`model_information` / `l3_model_record`）:**
  - `model_information::device_receiver_information` により、デバイス機能（`function`）ごとの受信部数確定フラグ（`receiver_count_known`）および受信部配列（`receiver_information`）を定義。
  - 各受信部は、筐体内一意のインデックス（`index`）、親デバイス内ローカルインデックス（`local_index`）、対応放送方式マスク（`systems`、`1 << dtv_system` の和）、フレームストライド・同期オフセット、対応操作ビットマスク（`known_operations`, `supported_operations`）、およびTSスクランブル状態（`ts_scrambling`、既定 `DTV_TS_UNKNOWN`）を保持。
  - `DTV_DEFINE_MODEL` マクロを通じて `l3_model_record::information` へ接続。実機5製品の受信部表は変更・推測せず、合成機種および将来の機種宣言用の拡張基盤を構成。
- **公開受信部配列とポインタの安定性（正本§4.2・§4.4遵守）:**
  - `dtv_get_receivers(dtv_device *object)` は、親デバイスに属する受信部ポインタ列を返す。
    - **数未知:** `receiver_count_known == false` の間は `NULL` を返す。
    - **既知0件:** `receiver_count_known == true` かつ受信部数 0 の場合は、先頭が `NULL` である配列ポインタ（`{nullptr}`）を返す。
    - **既知複数:** 受信部ポインタ列の末尾に `NULL` を付与した終端配列ポインタを返す。
  - **同一接続（同一root）におけるポインタ安定性とroot所有:**
    - `Root` が `Device` を所有し、`Device` が `Receiver` を所有する。
    - 同一接続である限り、再 `dtv_list_enclosures`、`dtv_open_enclosure`、`dtv_close_enclosure`、および抜去後の `dtv_hold_enclosure` 保持中も、同一の `dtv_receiver` ポインタが維持される。
    - 機器抜去後の再接続時は、新しい `Root`、`Device`、および新しい `dtv_receiver` オブジェクトが割り当てられ、旧世代のポインタとは同一視されない。
  - **公開getterのI/O・確保フリー特性:**
    - `dtv_get_receivers`、`dtv_get_index`、`dtv_get_local`、`dtv_get_systems`、`dtv_get_ts_scrambling`、および `dtv_get_receiver_support` は、事前確保済みの内部状態を参照し、USB I/Oや動的メモリ確保を行わない。
- **受信部操作判定（`dtv_get_receiver_support`）:**
  - `DTV_OP_LNB_POWER` から `DTV_OP_LNA` までの操作種別に対し、受信部の宣言粒度で判定。
  - 操作が未申告（`known_operations` の該当ビットが 0）の場合は `DTV_UNKNOWN` を返す。
  - 操作が申告済みの場合は、`supported_operations` の該当ビットに応じて `DTV_YES` または `DTV_NO` を返す。引数不正時は `DTV_ERROR_INVALID_ARGUMENT` を設定。
- **動的確定接点とアトミック反映（`EnclosureRegistry::update_receiver_declarations`）:**
  - 親デバイスの選定や遅延identify後に受信部宣言を動的に確定するため、caller boundaryで呼び出す私有opt-in関数 `update_receiver_declarations` を追加。
  - **検証とアトミック性（全検証・事前確保後の反映）:**
    - 対象筐体が `DTV_ENCLOSURE_READY` であることを確認。
    - 重複index、親デバイス未選定、不正systems、または既に受信部数が確定しているデバイスへの再更新などの不整合を事前検証し、違反時は `DTV_INTERNAL_RESULT_INVALID_ARGUMENT` で拒否（既知表不変）。
    - 全ての `Receiver` オブジェクトおよび公開ビュー配列の事前確保を含め、宣言検証・事前確保・一括反映（commit）まで `EnclosureRegistry::State::mutex`（`state_->mutex`）を保持して実行する（`Root` 自体には mutex メンバは存在しない）。
    - 確保途中の失敗（OOM等）時は例外を捕捉して既存状態を一切変更せず（未知のまま維持）`DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED` を返す。検証と確保が完了した完成表のみを一括反映（swap）し、途中状態や部分配列を公開しない。
  - **合成テストbridgeと実製品の区別:**
    - テスト用bridge `update_public_discovery_receiver_declarations_for_test` により、合成機種における未知→既知0件およびOOMアトミック性を検証。実機製品identify情報の供給元は未接続（後続工程）。
- **同期境界・並行性と決定47:**
  - 呼び出し側は決定47に基づき、`dtv_list_enclosures`、`dtv_wait_event`、配列読み取り、および配列要素の `dtv_hold_enclosure` を直列化する義務がある。
  - hold済みの筐体・デバイス・受信部に対する各 `dtv_get_` getter関数は、内部の短いmutex保護によりスレッドセーフに並行呼び出し可能である。
  - 未確認の並行安全（呼び出し側直列化前提を緩和するなど）を過剰に保証しない。
- **PR34操作受付target identityとの接続と未接続境界:**
  - PR34の私有操作受付において、`PublicOperationRequest::target_kind == stream` / `receiver` の際に、公開 `dtv_get_index` の値を `target_id` として渡すことで、特定receiverを対象とした排他ticket予約（同一receiver直列化、別receiver並行実行）への経路を接続。
  - **後続工程との境界:**
    - 公開受信部操作API（`dtv_tune`, `dtv_tune_by_frequency`, `dtv_read_stream` 等）、LOCKイベントプロデューサー（`DTV_EVENT_LOCK_ACQUIRED` / `LOCK_LOST`）、カードAPDU送受信、および実機5製品の受信部数・方式・パラメータの実機宣言は未接続であり後続工程。

## 公開LOCK通知とイベント配送（合成共有IN接続）

PR37では、開いた筐体の継続共有INから得られる受信部のロック変化通知を、公開イベントFIFO（`dtv_wait_event`）へ接続する機構を追加している。

- **ロック通知源の指定と検証（`PublicOpenPlan::receiver_lock_source`）:**
  - open計画（`PublicOpenPlan`）において `receiver_lock_source = true` を指定したendpointをロック通知の入力源として特定・登録（`SharedInOwner::lock_sources`）。`shared_in_depth == 0` での指定は `UNSUPPORTED`（拒否）。
  - 継続共有INのworker上で `open_ops->demux` が `PublicOpenMessageKind::notification` かつ `notification_kind == PublicOpenNotificationKind::receiver_lock` を返した際、許可されたendpointおよびmessage generationの合致を検査。未許可sourceや旧generationは破棄し共有INは継続。
- **受信部ロック状態の観測と検証（`EnclosureRegistry::observe_receiver_lock`）:**
  - `observe_receiver_lock` は `EnclosureRegistry::State::mutex`（`state_->mutex`）の保護下で実行される（`Root` 自体には mutex メンバは存在しない）。
  - **不適合通知の破棄:**
    - 筐体状態が READY でない（切断時等）→ `DISCONNECTED`（共有IN処理はOKで継続）
    - 不正な lock_state 値（`lock_state > 1`）、親デバイスの接続epoch不一致、または存在しない `receiver_index` → `INVALID_ARGUMENT`（破棄し共有INは継続）
    - 同一値の再通知（`observed.lock_known && observed.lock_state == lock_state`）→ イベント発行なしで正常復帰
- **初回既知観測時の振る舞い（正本L3決定52）:**
  - 正本L3決定52（`spec/dtv11-l3-spec.md` §7、`l3/dtv.h`、`l3/decisions.md`）に基づき、各受信部のロック状態が初めて判明した際（値が 0 でも 1 でも）は、その値を初期の基準値として記録するのみとし、`DTV_EVENT_LOCK_ACQUIRED` および `DTV_EVENT_LOCK_LOST` イベントは発行しない。
  - 初回観測以降に、0 から 1 へ変化した際に `DTV_EVENT_LOCK_ACQUIRED`、1 から 0 へ変化した際に `DTV_EVENT_LOCK_LOST` を発行する。
  - **注記:** 初回のロック状態は公開 `dtv_get_signal().lock` で照会できる仕様となっており、PR38にて `dtv_get_signal()` 公開APIが接続され、初回イベント非発行時であっても現在値が照会可能となった。
- **イベント生成・FIFO接続と参照寿命:**
  - 生成される `dtv_event` の `enclosure` は同一root筐体を指し、`receiver` は `dtv_get_receivers` で得られる同一接続の安定した公開 `dtv_receiver` ポインタを指す（正本§7・要件F-16遵守）。
  - イベントキュー（`QueuedEvent`）は、root参照（`event_references`）に加え、対象receiverを所有するrootのエイリアス参照（`receiver_owner`）を保持し、`dtv_wait_event` で返却された後も次の任意の一覧関数または `dtv_wait_event` の公開caller境界まで対象オブジェクトの寿命を維持する（正本§4.3遵守）。
  - CARDイベントと同一の順序付きFIFO（`state_->events`）へ追加され、`dtv_wait_event` のcaller境界で順序通り配送される。
- **同期境界・停止時挙動と決定50（OOM時のsticky RESOURCE_LIMIT）:**
  - 呼び出し側は決定47に基づき `dtv_wait_event` 等の呼び出しを直列化する義務がある。
  - 通常の `cancel_enclosure` は実行中操作のみを停止し、共有IN subscriptionおよびLOCK通知購読は継続する。
  - `dtv_close_enclosure` 時は新規通知を遮断し、遅延callback完了（drain）までPENDINGとしてroot・receiver・event参照を保持し、CLOSED後に解放する。
  - イベント追加時のメモリ確保失敗時は、最新のロック状態（`lock_known` / `lock_state`）は維持しつつ、既保存イベント（CARD/LOCK等）の配送完了後に `dtv_wait_event` が同一セッションで `DTV_ERROR_RESOURCE_LIMIT` を返し続ける（決定50）。
- **後続工程との境界:**
  - 実機5製品のロック観測source endpoint・通知バイト・閾値、校正CNR/電力値測定、選局（`dtv_tune`）、ストリーム受信（TS）、tune/stream世代生成・リセット、カードAPDU送受信は本PR（PR37）の対象外であり後続工程。

## 公開シグナル状態取得と受信部ロック現在値接続（PR38、合成公開C経路）

PR38では、開いた筐体配下の各受信部について、現在のシグナル測定値（ロック状態、C/N比、信号電力）を照会する公開関数 `dtv_get_signal()` を接続した。

- **公開API入口と呼び出し規約:**
  - 公開シグネチャ: `int32_t dtv_get_signal(dtv_receiver *object, const dtv_call_options *options, dtv_signal *out, dtv_error *error)`。
  - 引数検証: `object == nullptr || out == nullptr`、または `options != nullptr && options->struct_size < sizeof(*options)` の場合は `DTV_ERROR_INVALID_ARGUMENT` を返す。
  - C++例外境界: 実装内部の全例外を `try ... catch (...)` で捕捉し、C API境界を越えないよう遮断してエラーコードへ変換する。
  - 成功時のみ更新: 呼び出し側から渡された `*out` は、事前検証、排他同期、操作チケット取得、状態スナップショット、および完了検証のすべてが正常終了した場合に限り `signal.struct_size = sizeof(signal)` を設定して書き込む。途中で失敗・キャンセルされた場合は呼び出し側の `*out` を変更しない。
- **排他同期・判定順序とスナップショット:**
  - `EnclosureRegistry::State::mutex`（`state_->mutex`）の保護下で `receiver_signal_snapshot` を呼び出し、receiver-owned なロック状態（`lock_known`, `lock_state`）、所属接続epoch、receiver index、親root状態、および `DTV_OP_SIGNAL` 宣言有無をアトミックにスナップショットする（特定のmutex所有者や未測定保持時間の保証は行わない）。
  - 事前判定順序:
    1. 切断状態（`enclosure_status == DTV_ENCLOSURE_DISCONNECTED`）→ `DTV_ERROR_DISCONNECTED`
    2. 宣言NO（`signal_support == DTV_NO`、`DTV_OP_SIGNAL` bitがNO）→ `DTV_ERROR_UNSUPPORTED`
    3. 未宣言・未準備（`signal_support != DTV_YES`、`DTV_UNKNOWN`）→ `DTV_ERROR_NOT_READY`
  - open 状態判定: open slot が存在しない、または open phase が pending / closed / closing の場合は `DTV_ERROR_NOT_READY`。
- **既存操作チケット（operation ticket）による直列化・競合調停:**
  - 既存の receiver 単位操作チケット機構（`slot->operations`）に登録し、正本§9（同一受信部操作の呼び出し側順序、異なる受信部間の並行、および共有USB操作の内部調停）に従って直列化・調停する。
  - `dtv_call_options` から clock および timeout（monotonic deadline）を取得し待機する。
  - キャンセル（`operation->cancelled`）検出時は `DTV_ERROR_CANCELLED` を返す。
  - close 中（`slot->operation_closing`）検出時は `DTV_ERROR_NOT_READY` を返す。
  - deadline 超過時は `DTV_ERROR_TIMEOUT` を返す。
  - 操作チケット獲得後に root の open phase、切断、宣言NO、未準備を再検証する。
- **測定値の状態（`state`）と理由（`reason`）の対応（正本決定53）:**
  - 正本決定53（`spec/dtv11-l3-spec.md` §6.1/§6.5/§7、`l3/dtv.h`、`l3/decisions.md`）に基づき、Core 公開ヘッダ（`include/dtv/dtv.h`）に同期された `enum dtv_measurement_state` および `enum dtv_measurement_reason` に従う:
    - `state`: `DTV_MEASUREMENT_UNKNOWN` (0), `DTV_MEASUREMENT_UNSUPPORTED` (1), `DTV_MEASUREMENT_VALID` (2)
    - `reason`: `DTV_MEASUREMENT_REASON_NONE` (0), `DTV_MEASUREMENT_REASON_NOT_SAMPLED` (1), `DTV_MEASUREMENT_REASON_STALE` (2), `DTV_MEASUREMENT_REASON_TUNE_CHANGED` (3), `DTV_MEASUREMENT_REASON_DEVICE_LOST` (4), `DTV_MEASUREMENT_REASON_READ_FAILED` (5)
    - `reason` は `state` が `UNKNOWN` のときのみ意味を持ち、`UNSUPPORTED` および `VALID` のときは常に `DTV_MEASUREMENT_REASON_NONE` (0) とする。
- **ロック測定値（`signal.lock`）の現在値照会:**
  - 初回未観測時: `state = DTV_MEASUREMENT_UNKNOWN`, `reason = DTV_MEASUREMENT_REASON_NOT_SAMPLED`, `value = 0`。
  - 観測後（既知0または1）: `state = DTV_MEASUREMENT_VALID`, `reason = DTV_MEASUREMENT_REASON_NONE`, `value = 0` または `1`。
  - **決定52との連携:** 正本決定52に基づき、初回観測値（0でも1でも）はイベント（`LOCK_ACQUIRED`/`LOCK_LOST`）を発行しない基準値として記録されるが、`dtv_get_signal()` を呼ぶことで、初回イベント非発行時であっても現在判明しているロック状態（0または1）を即座に照会できる。
  - **決定50（OOM時）との連携:** イベントジャーナル確保失敗時（sticky `RESOURCE_LIMIT` 状態）でも、receiver に保持された最新のロック現在値は破棄されず、`dtv_get_signal()` により照会可能である。
- **未校正測定値（C/N比・信号電力）の振る舞い:**
  - C/N比（`signal.cnr_mdb`）および信号電力（`signal.signal_mdbm`）は、実機固有の校正入力・測定パスが存在しないため、正本§6.5・決定53に従い `state = DTV_MEASUREMENT_UNSUPPORTED`, `reason = DTV_MEASUREMENT_REASON_NONE` を返す（校正していない生の値は返さない）。
- **後続工程との境界:**
  - 本PR（PR38）で接続されたのは、合成C経路における公開 `dtv_get_signal` の現在値照会（receiver-owned lock snapshot、open/チケット境界、決定53 reason同期）である。
  - 実機5製品固有のlock source endpoint・通知バイト・閾値判定、校正CNR/電力値測定、選局（`dtv_tune`）、ストリーム受信（TS）、tune/stream世代生成・リセット、カードAPDU送受信、daemon/wireパスは本PRの対象外であり後続工程である。

## 公開周波数選局と選局時ロック状態遷移・世代フェンス（PR39、合成機種SPI接続）

PR39では、開いた筐体配下の受信部に対して、放送方式と周波数を指定して選局を行う公開関数 `dtv_tune_by_frequency()`、機種私有SPI（`plan_tune`, `tune_by_frequency`）、作用開始前コールバック（`begin_effect`）、操作エポック管理、および選局時のロック状態失効・再観測イベント接続（正本決定54）を接続した。

- **公開API入口・引数検証と宣言能力確認:**
  - 公開シグネチャ: `int32_t dtv_tune_by_frequency(dtv_receiver *object, uint32_t system, uint64_t frequency_hz, const dtv_tune_options *options, const dtv_call_options *call_options, dtv_error *error)`。
  - 引数検証:
    - `object == nullptr`、放送方式が定義範囲外（`system > DTV_SYSTEM_ISDB_S`）、または `call_options != nullptr && call_options->struct_size < sizeof(*call_options)` の場合は `DTV_ERROR_INVALID_ARGUMENT` を返す。
    - `options != nullptr` の場合、`options->struct_size < sizeof(*options)`、セレクタ種別が定義範囲外（`options->selector > DTV_SELECTOR_TSID`）、またはLNA設定が定義範囲外（`options->lna > DTV_LNA_ON`）であれば `DTV_ERROR_INVALID_ARGUMENT` を返す。
  - C++例外境界: 実装内部の全例外を `try ... catch (...)` で捕捉し、C API境界を越えないよう遮断してエラーコードへ変換する。
  - 状態・宣言能力の事前確認:
    - 切断状態（`enclosure_status == DTV_ENCLOSURE_DISCONNECTED`）→ `DTV_ERROR_DISCONNECTED`。
    - 方式非対応（`receiver_systems` の該当方式bitが0）→ `DTV_ERROR_UNSUPPORTED`。
    - 方式別選局操作（ISDB-Tなら `DTV_OP_TUNE_ISDB_T`、ISDB-Sなら `DTV_OP_TUNE_ISDB_S` 等）の宣言能力: 未宣言（known bitなし）は `DTV_ERROR_NOT_READY`、非対応（supported bitなし）は `DTV_ERROR_UNSUPPORTED`。
    - TSIDセレクタ指定時（`options->selector == DTV_SELECTOR_TSID`）の宣言能力（`DTV_OP_SELECT_TSID`）: 未宣言は `DTV_ERROR_NOT_READY`、非対応は `DTV_ERROR_UNSUPPORTED`。
    - open 状態判定: open slot が存在しない、または open phase が pending / closed / closing の場合は `DTV_ERROR_NOT_READY`。
- **私有SPI（`plan_tune`）と機種値域判定（正本決定48）:**
  - 機種の私有SPI `ops->plan_tune` を呼び出し、引数の妥当性、準備状態、対応状態、および物理共有リソースの排他キー（`resource_keys`）を計画する。
  - 衛星の slot 等が機種のサポート範囲外（決定48）である場合は、`DTV_ERROR_UNSUPPORTED`（終了コード3）を返す。
  - `plan.ready == false` は `DTV_ERROR_NOT_READY`、`plan.supported == false` は `DTV_ERROR_UNSUPPORTED` を返す。
- **並行規則と操作チケット（正本§9）:**
  - 正本§9（同一受信部操作の呼び出し側順序、異なる受信部間の並行、および共有USB操作の内部調停）に従い、既存の receiver 単位操作チケットおよび共有リソース競合キー（`slot->operations`）に登録して直列化・競合調停を行う。
  - `call_options` から clock および timeout（monotonic deadline）を取得し待機する。
  - キャンセル（`operation->cancelled`）検出時は `DTV_ERROR_CANCELLED` を返す。
  - close 中（`slot->operation_closing`）検出時は `DTV_ERROR_NOT_READY` を返す。
  - deadline 超過時は `DTV_ERROR_TIMEOUT` を返す。
  - チケット獲得後に root の open phase および切断状態を再検証する。
- **私有選局SPIの呼び出しと作用開始境界（`begin_effect`）・決定54:**
  - caller thread 上で機種の私有SPI `ops->tune_by_frequency` を実行する。
  - 機種は、機器に対する実際の作用（ハードウェアレジスタ設定や選局要求送信等）を開始する直前に、提供された `session.begin_effect` コールバックを呼び出す。
  - `begin_receiver_tune`:
    - `state_->mutex` 保護下で実行する（特定のmutex所有者や未測定保持時間の保証は行わない）。
    - 切断なら `DISCONNECTED`、接続epoch不一致や操作epoch不一致なら `CANCELLED` を返す。
    - 操作エポックを前進させる（`++receiver->operation_epoch`）。
    - ロック状態を `lock_known = false`, `lock_reason = DTV_MEASUREMENT_REASON_TUNE_CHANGED` へ更新し、シグナル照会値を `UNKNOWN / TUNE_CHANGED` へ失効させる。
    - **決定54（選局し直したときのロック基準）:** ロックイベント比較基準（`lock_baseline_known`, `lock_baseline_state`）は選局し直しても消さずに保持する。決定52の「初回」は受信部ごとに1回だけであり、選局毎にリセットしない。
- **作用前拒否と作用後失敗・キャンセルの区別:**
  - **作用前拒否（pre-effect refusal）:** `begin_effect` 呼出前の引数検証・宣言能力・機種値域・チケット待機中のエラーで拒否された場合は、ロック状態・理由・エポックは変更されず、前の値のまま維持される（決定54）。
  - **作用後失敗・キャンセル・期限切れ:** `begin_effect` 呼出後に I/O 失敗、キャンセル、またはタイムアウトとなった場合は、新エポックの観測値が届くまでロック状態は `UNKNOWN / TUNE_CHANGED` を維持する。
- **操作エポックフェンス（epoch fencing）と再観測・LOCKイベント:**
  - 継続共有 IN の demux から届くロック観測通知について、古い操作エポックの通知（`observed.operation_epoch != operation_epoch`）は破棄・無視される（フェンス）。
  - 現在の操作エポックに合致する観測値が届いた際:
    - 初回観測（受信部として初回の既知観測、`lock_baseline_known == false`）であれば、基準値として記録するのみでイベントは発行しない（決定52）。
    - 既に基準値が存在する場合（決定54）:
      - 保持されていた基準値と新観測値を比較し、0→1なら `DTV_EVENT_LOCK_ACQUIRED`、1→0なら `DTV_EVENT_LOCK_LOST` を発行する。同値ならイベントは発行しない。
      - 基準値（`lock_baseline_state`）を新観測値で更新する。
    - ロック状態は `lock_known = true`, `lock_state = lock_state`, `lock_reason = DTV_MEASUREMENT_REASON_NONE` となり、`dtv_get_signal()` は `VALID / NONE` を返すようになる。
- **close PENDING と参照回収:**
  - close 中は新規選局要求を `DTV_ERROR_NOT_READY` で拒否する。実行中選局は close の drain 待機・キャンセルと連携する。
  - イベント参照回収境界（任意の一覧関数または `dtv_wait_event`）は維持される。
- **後続工程との境界:**
  - 本PR（PR39）で接続されたのは、合成機種SPIを介した周波数選局（`dtv_tune_by_frequency`）、操作エポック管理、および選局時ロック状態遷移（決定54）である。
  - channel resolver（チャンネル文字列から周波数・方式への変換、`dtv_tune_by_channel`）、ストリーム受信（`dtv_open_stream`, `dtv_read_stream`）、TSパケット処理、カードAPDU送受信、実機5製品固有の tune コマンド / 実機パラメータ / ハードウェア通知フェンスは本PRの対象外であり後続工程である。

## 公開チャンネル選局と周波数・セレクタ解決（PR40、共通選局経路）

PR40では、公開C API関数 `dtv_tune_by_channel()` による厳密なチャンネル文字列の構文解析、周波数・セレクタの解決（正本決定55、L1 §10）、および既存の公開周波数選局（`dtv_tune_by_frequency`）共通経路への接続を実装した。

- **公開API入口と引数検証:**
  - 公開シグネチャ: `int32_t dtv_tune_by_channel(dtv_receiver *object, const char *channel, const dtv_tune_options *tune_options, const dtv_call_options *options, dtv_error *error)`。
  - 引数検証:
    - `object == nullptr`、`channel == nullptr`、または `options != nullptr && options->struct_size < sizeof(*options)` の場合は `DTV_ERROR_INVALID_ARGUMENT` を返す。
    - `tune_options != nullptr` の場合、`tune_options->struct_size < sizeof(*tune_options)`、セレクタが定義範囲外（`selector > DTV_TUNE_SELECTOR_TSID`）、LNAが定義範囲外（`lna > DTV_LNA_ON`）、または `selector == DTV_TUNE_SELECTOR_SLOT && value > 11`（slotの物理定義範囲外）であれば `DTV_ERROR_INVALID_ARGUMENT` を返す。
- **厳密なASCIIチャンネル構文解析と換算式（正本決定55、L1 §10）:**
  - ASCII 10進パーサーによりオーバーフロー安全にチャンネル番号を解析する。
  - 地上波: `13`〜`62`（2桁数値）または `T13`〜`T62`（'T'接頭辞+2桁数値）。
    - 方式: `DTV_SYSTEM_ISDB_T`。
    - 整数Hz計算式（決定55）: `473142857 + (ch − 13) × 6000000` Hz。
  - BS: `BS01`〜`BS23`（奇数番号のみ、2桁0埋め必須）、任意で接尾辞 `_0`〜`_11`（アンダースコア+0〜11、先頭0埋めなし）。
    - 方式: `DTV_SYSTEM_ISDB_S`。
    - 整数Hz計算式（決定55）: `1049480000 + ((ch − 1) / 2) × 38360000` Hz。
    - 衛星周波数は、LNB 変換後の中間周波数（IF）である。
  - CS: `CS2`〜`CS24`（偶数番号のみ）、および `CS02`〜`CS08`（1桁偶数の0埋めも受容）。
    - 方式: `DTV_SYSTEM_ISDB_S`。
    - 整数Hz計算式（決定55）: `1613000000 + ((ch − 2) / 2) × 40000000` Hz（衛星IF）。
  - 定義外の文字、非正規なパディング、範囲外数値、不正な接尾辞等は `DTV_ERROR_INVALID_ARGUMENT` で即座に拒否する。
- **セレクタの既定と排他（正本決定55、L1 §10）:**
  - 地上波: セレクタ（SLOT または TSID）が指定された場合は `DTV_ERROR_INVALID_ARGUMENT` で拒否する。
  - 接尾辞付きBS（`BSxx_s`）: 接尾辞の slot（`s`）を採用し、明示的なセレクタが指定された場合は `DTV_ERROR_INVALID_ARGUMENT` で拒否する。
  - 接尾辞なしBS（`BSxx`）: 明示的なセレクタ（SLOT または TSID）の指定が必須であり、未指定（`DTV_TUNE_SELECTOR_NONE`）の場合は `DTV_ERROR_INVALID_ARGUMENT` で拒否する。
  - CS: セレクタが未指定（`DTV_TUNE_SELECTOR_NONE`）の場合は既定で slot 0（`DTV_TUNE_SELECTOR_SLOT`, `value = 0`）を設定する。明示的なセレクタ（SLOT または TSID）があればそれを採用する。
  - LNA 設定（`tune_options->lna`）は解決後もそのまま保持される。
  - **TSID数値範囲の境界:** 正本L3 §6.5および決定55に基づき、公開CチャンネルAPI入口では明示されたTSID値をそのまま既存周波数選局APIへ転送する（L1 §10の 1..65534 は衛星周波数直接指定におけるCLI規定であり、C入口で新規の独自範囲チェックを追加せず既存周波数パスと整合）。
- **既存 `dtv_tune_by_frequency` 共通経路への合流:**
  - 解決された方式、整数Hz周波数、オプション、およびコールバックオプションは、そのまま既存の公開関数 `dtv_tune_by_frequency()` へ渡される。
  - 方式・宣言能力確認、機種SPI（`plan_tune`、決定48範囲外slot判定）、正本§9並行規則に基づく操作チケット直列化・調停、私有選局SPI、`begin_effect` コールバック、操作エポック前進、シグナル照会値の `UNKNOWN / TUNE_CHANGED` 失効、決定54（最後既知イベント比較基準保持、選局毎初回リセットなし、旧エポック通知フェンス）、新エポック観測復帰とLOCKイベント発行、close PENDING連携のすべてを共通して利用する。別の状態機械は一切作成しない。
- **作用前拒否と作用後失敗の区別:**
  - チャンネル構文エラー、セレクタ競合・不足、引数不正等により解決・受付段階で拒否された場合（作用前拒否）は、`begin_effect` や周波数選局のハードウェア作用には進まないため、ロック状態・理由・エポックは変更されず前の値のまま維持される。
- **後続工程との境界:**
  - 本PR（PR40）で接続されたのは、公開チャンネル選局（`dtv_tune_by_channel`）、厳密なチャンネル解決（決定55）、および既存周波数選局共通経路への合流である。
  - ストリーム受信API（`dtv_open_stream`, `dtv_read_stream`）、TSパケット処理、カードAPDU送受信API、実機5製品固有の tune コマンド / 実機パラメータ / 機器丸め / RF変換・ハードウェア通知フェンス、CLIチャンネル実行の統合は本PRの対象外であり後続工程である。

## 公開ストリーム受信と有限188/192バイトTS変換・決定56/57連続キューとPIDフィルタ接続（PR41/PR42/PR43/PR44、合成共有IN・TsFramer接続）

開いた筐体配下の受信部からTSストリームを開いて読み取る公開C API関数群（`dtv_open_stream`, `dtv_read_stream`, `dtv_cancel_stream`, `dtv_close_stream`, `dtv_get_stream_close_state`, `dtv_release_stream`, `dtv_get_dropped_bytes`, `dtv_get_desyncs`, `dtv_set_pid_filter`）、既存の共有INワーカー（`SharedInOwner`）へのストリームシンク接続、Core `TsFramer` によるパケット同期、192/4プレフィックス変換、受信部別有界キューと決定56のSLOW_CONSUMER終端、決定57に基づく機器側PIDフィルタ設定と新着選別、およびストリーム独立ライフサイクルを接続した。

- **公開ストリームAPI群と引数・状態検証:**
  - `dtv_open_stream(dtv_receiver *receiver, const dtv_call_options *options, dtv_stream **out, dtv_error *error)`:
    - 引数検証: `receiver == nullptr || out == nullptr`、または `options != nullptr && options->struct_size < sizeof(*options)` の場合は `DTV_ERROR_INVALID_ARGUMENT` を返す。失敗時は `*out = nullptr` とする。
    - 親enclosure状態: 親筐体が切断状態であれば `DTV_ERROR_DISCONNECTED`、親筐体が開いた状態（open phase が `Phase::open`）でなければ `DTV_ERROR_NOT_READY` を返す。
    - ストライド/同期オフセット検証: 受信部メタデータから `source_stride_bytes` と `source_sync_offset_bytes` を取得し、`(188, 0)` または `(192, 4)` のみを受け付ける（未設定やその他は `DTV_ERROR_NOT_READY` / `DTV_ERROR_UNSUPPORTED`）。192バイトストライド時は機種SPI `transform_stream` が必須。
    - 機種ストリーミング能力（`model->streaming_support` 等）を確認し、機種SPI `plan_stream` によりリソースキー（最大8個）を宣言取得、ソート・重複排除および妥当性検証（0値拒否）を行い、既存の操作受付（`OpenSlot::OperationControl`）を通じてチケットを取得・保持する。続いて機種SPI `prepare_stream` および `start_stream` を呼び出す。
    - 成功時は一意のストリーム世代（`stream_generation`）を発行し、内部バッファに私有キュー容量（`queue_capacity`、1MiB）を `reserve` して再アロケーションを抑止した上で、親enclosureの参照を保持する `dtv_stream` ハンドルを構築して `*out` に設定する。
  - `dtv_set_pid_filter(dtv_stream *stream, const uint16_t *pids, uint32_t count, dtv_error *error)`:
    - 引数検証: `stream == nullptr || stream->state == nullptr`、または `count != 0 && pids == nullptr` の場合は `DTV_ERROR_INVALID_ARGUMENT` を返す。各 PID が 13 ビット範囲（`pids[index] > 0x1fff`）を超える場合も `DTV_ERROR_INVALID_ARGUMENT` を返す。
    - ストリーム状態・エポック検証: ストリームが `closed || closing || cancelled` の場合は `DTV_ERROR_CANCELLED`、`terminal` の場合は `DTV_ERROR_NOT_READY` を返す。親筐体が切断状態または接続エポック不一致の場合は `DTV_ERROR_DISCONNECTED` を返す。
    - 能力照会（`DTV_OP_OUTPUT_PID_FILTER`）: 受信部サポートが `DTV_SUPPORT_NO` の場合はハードウェア I/O を行わず直ちに `DTV_ERROR_UNSUPPORTED` を返す。`DTV_SUPPORT_YES` 以外は `DTV_ERROR_NOT_READY` を返す。
    - 三層機種dispatch: `dtv::internal::dispatch_set_pid_filter` 経由で機種関数を呼び出し、既存の操作チケットおよび SharedInOwner 制御通信を通じて機器へ設定する。
  - `dtv_read_stream(dtv_stream *stream, uint8_t *buffer, uint64_t capacity, const dtv_call_options *options, uint64_t *out_size, dtv_error *error)`:
    - 引数検証: `stream == nullptr || stream->state == nullptr || out_size == nullptr`、`capacity != 0 && buffer == nullptr`、または `options != nullptr && options->struct_size < sizeof(*options)` の場合は `DTV_ERROR_INVALID_ARGUMENT` を返す。初期状態として `*out_size = 0` を設定する。
    - capacity検証: `capacity < 188` の場合、最小TSパケットサイズ未満のため `DTV_ERROR_BUFFER_TOO_SMALL` を返す。
    - 読み取り待機とブロック解除: データ到着まで条件変数で待機し、タイムアウト時は `DTV_ERROR_TIMEOUT`、明示的なキャンセル呼出（`dtv_cancel_stream`）時やストリームクローズ（`dtv_close_stream`）時だけでなく、親筐体の明示クローズ（`dtv_close_enclosure`）時にも直ちに `DTV_ERROR_CANCELLED` で起床する（正本 L3 §6.2・§6.6）。
    - 物理抜去との区別: 呼出し前に既に物理抜去されている場合の判定は `DTV_ERROR_DISCONNECTED`（L3 §6.1 step 1）であり、明示クローズ（= `CANCELLED`）と明確に区別する。なお、ブロック待機中に物理抜去が発生した場合の起床エラーコードは正本引用条項で明示規定されていないため、独自に確定しない。
    - パケット配送: `TsFramer` で同期・変換された完全な 188 バイト整数の倍数パケットのみを `buffer` にコピーし、書き込んだバイト数を `*out_size` に設定する。有限ソース終端（EOF）到達時は `*out_size = 0`（`DTV_ERROR_OK`）を返す。キュー満杯による終端ラッチ後は、保存済みパケットをすべて返し終えた後の読取で `*out_size = 0` と `DTV_ERROR_SLOW_CONSUMER` (22) を返す（正常EOFとは明確に区別）。
  - `dtv_get_dropped_bytes(dtv_stream *stream)` / `dtv_get_desyncs(dtv_stream *stream)`:
    - `TsFramer` が記録した破棄バイト数および同期外れ（desync）回数を照会する。
  - `dtv_cancel_stream(dtv_stream *stream, dtv_error *error)`:
    - ストリームのキャンセル状態をアトミックに設定し、進行中のストリーム操作（PID設定等）をキャンセルしてブロック待機中の reader を起床させて `DTV_ERROR_CANCELLED` で安全に終了させる。
  - `dtv_close_stream(dtv_stream *stream, const dtv_call_options *options, dtv_error *error)` / `dtv_get_stream_close_state(dtv_stream *stream, dtv_error *error)` / `dtv_release_stream(dtv_stream *stream)`:
    - 親enclosureと同様の独立した閉鎖ライフサイクル（正本§6.6）。
    - close開始で新規読取は遮断され、待機中 reader およびストリーム操作はキャンセル起床する。機種SPI `stop_stream` および `after_stream_drain` を実行し、期限内完了で `DTV_CLOSE_CLOSED` (2)、drain待機中は `DTV_CLOSE_PENDING` (1) を返す。
    - `dtv_get_stream_close_state` でポーリングおよび再試行が可能。
    - 完全に CLOSED に達した後に `dtv_release_stream` を呼ぶことでストリームハンドルを解放する（PENDING 中の解放呼出は安全に無視される）。
    - ストリームを close / release しても、親enclosureのオープン状態は維持される（独立）。
- **共有IN・Core TsFramer・192/4プレフィックス変換とパケット配送:**
  - 既存の `SharedInOwner` ワーカーおよび共有IN受信経路にストリームシンクを接続。
  - **フレーミングと製品変換の責任分離（一か所所有）:**
    - Coreの責務: `TsFramer` が completion 境界に依存せず raw ソースチャンクからキャリーを保持しつつ、指定のストライド・同期オフセット（188/0 または 192/4）で先頭 0x47 同期ユニットを整列。コールバック前後の厳密な容量（consumed/produced）および先頭 0x47 の TS sync byte を検証し、公開キューへの追加および統計カウンタ（dropped_bytes, desyncs）を所有・更新する。
    - 製品SPI（`transform_stream`）の責務: 整列された 192 バイトユニットから先頭 4 バイトのタイムスタンププレフィックスを除去し、188 バイト TS パケットへ変換する単一の変換責任のみを所有する。
  - **188/0 ダイレクトコピーと 192/4 変換:**
    - 188/0 ソースの場合: 製品 transform コールバックを呼び出さず、整列済みパケットをダイレクトコピーしてオーバーヘッドを排除。
    - 192/4 ソースの場合: Core 整列後に `transform_stream` を1回呼び出し、先頭 4 バイトプレフィックスを除去して 188 バイト TS パケットを生成。
  - **分割 completion とノイズ耐性（カウンタ挙動）:**
    - 17 バイト等の細片化された分割 completion メッセージであっても、TsFramer の carry バッファにより正しく 192 バイトユニットへ再構成される。
    - 正常なプレフィックス除去（4バイト）は破棄バイト数（`dropped_bytes`）には計上されない。
    - 先頭 3 バイトノイズ混入時は `dropped_bytes = 3, desyncs = 0`、その後に不正同期ユニット（192バイト）が混入した場合は再同期時に破棄され、`dropped_bytes = 195` (3 + 192)、`desyncs = 1` へ遷移し、後続の正常ユニット（先頭 0x47）の 188 バイト元ペイロードが欠損なく配送される。有限ソース終端（EOF）では `*out_size = 0`（`DTV_ERROR_OK`）を返す。
- **決定56に基づく連続TSストリームの有界キューとSLOW_CONSUMER終端（PR43）:**
  - **有界キューと完全パケット格納:** 各ストリームは既存の私有有界キュー（1024*1024 バイト = 1MiB）を持ち、オープン時に `reserve` される。完全な 188 バイト整パケット単位（`accepted = (std::min)(produced, available - available % 188)`）でのみキューへ追加され、部分パケットは公開キューへ格納されない。1MiB の場合、完全パケットは最大 5577 パケット（1048476 バイト）となり、末尾 100 バイトはパケットサイズ未満のため未使用となる（このパケット数や余りバイト数は私有実装および fixture の検証値であり、公開の性能 SLA や ABI 保証値ではない）。
  - **オーバーフローラッチと保存済み順序の厳密保持:** 次の完全 188 バイトパケットを保持する空き容量がない場合（`accepted != produced`），ストリームの終端状態（`terminal = true`, `terminal_error = DTV_ERROR_SLOW_CONSUMER`）をアトミックにラッチする。このラッチは、後続の `dtv_read_stream()` によってキューに空きが生じても解除されない。ラッチ以降に到着した新規 TS パケットは破棄され、キューへ混入しない。呼び出し側は、オーバーフロー発生前に保存されたパケット（0..5576 等）を元の順番およびバイト内容で欠損なく読み出すことができる。
  - **共有IN（SharedInOwner）の継続動作:** キュー満杯によりストリームが終端ラッチされた後も、共有INワーカーは USB completion の回収と demux を停止することなく継続する。同一エンドポイント上の制御応答、カード状態通知（CARD_INSERTED/REMOVED 等）、および別受信部宛ての TS パケット（192/4 等）の配送は妨げられず正常に進行する。終端したストリーム宛ての新規 source_chunk は無視され、コールバックは `DTV_INTERNAL_RESULT_OK` を返して他の処理を安全に継続する。
  - **フレーミング統計カウンタとの分離:** キュー満杯により受け入れられず破棄された TS パケットは、Core `TsFramer` の同期外れ・区切り合わせ統計（`dtv_get_dropped_bytes()`, `dtv_get_desyncs()`）には一切加算されない。フレーミングカウンタは raw ソースの同期状態のみを反映する。
  - **明示 close と世代フェンスによる再オープン:** `SLOW_CONSUMER` 終端に達した後も、ストリームは明示的な `dtv_close_stream()` / `dtv_get_stream_close_state()` / `dtv_release_stream()` によりクローズ・解放される（親 enclosure や別受信部ストリームを巻き込んで閉じることはない）。解放後に再度 `dtv_open_stream()` を呼び出すことで、新しい一意のストリーム世代（`stream_generation`）のもとで再オープンが可能となり、旧世代の遅延完了メッセージはフェンスにより安全に破棄される。
  - **既存経路の再利用:** Core 整列、機種 transform、キューイング、ライフサイクル、および正本§9 操作チケット機構をそのまま再利用しており、別状態機械や新規公開 ABI は一切追加しない。
- **決定57に基づく公開PIDフィルタ機器設定と新着選別（PR44）:**
  - **機器設定と新着選別の責任分離（Coreソフト単独選別との区別）:** PID フィルタは、機種が機器（ハードウェア）に設定するもの（L1 仕様「ハード能力差」および決定57）。Core 内部のソフトウェア選別だけで完結する構成ではなく、三層機種 dispatch・機種関数・SharedInOwner 制御通信を通じて合成機器へ設定要求が送られ、機器の ACK（制御応答）成功時に初めて機器側で新着 TS 送出の選別が有効化される。
  - **初期全通過・非空置換・count 0 解除の規則:**
    - ストリームオープン直後の初期状態は「全 PID 通過（全通過）」。
    - 非空配列（`count >= 1`）の設定成功時は、以前の指定を置換して指定 PID のみを選別通過。
    - `count == 0`（または空指定）の設定成功時は、フィルタを解除して全 PID 通過へ復帰。
  - **設定成功後の新着パケット適用と既存キューの順序・内容保持:**
    - 機器側フィルタリングが効くのは設定成功後に機器から新着した TS パケットからである。
    - 設定前に Core のキューに既に保存されていたパケットは、フィルタ対象外の PID であっても内容・順序がそのまま維持されて返される（決定56の保存済みパケット返却原則と整合）。
  - **除外パケットのフレーミング統計非計上:** 機器側でフィルタ除外されたパケットは、Core `TsFramer` の区切り合わせ破棄数（`dtv_get_dropped_bytes()`）や同期外れ回数（`dtv_get_desyncs()`）には一切加算されない。
  - **ライフサイクル・チケット・世代管理の整合:**
    - ストリーム操作チケット（`target_kind = stream`）とストリーム上の操作チケット（PID設定等）の競合を調停する `stream_lease` フラグを導入。
    - 設定中のキャンセル・クローズ時は `cancel_public_stream_operations` により in-flight 操作を安全にキャンセル。
    - ストリーム再オープン時に取り消し済み retired 出力操作がチケット枠を塞がないよう `reap_retired_operations` により回収。
    - 再オープン後は再び初期状態（全通過）で開始され、旧世代の遅延パケットは世代フェンスにより破棄される。
    - 決定56のキュー満杯（`SLOW_CONSUMER`）時や、PID設定進行中であっても、共有IN（SharedInOwner）での制御応答・カード通知・別受信部（192/4）への配送は妨げられず並行進行する。
- **並行規則・操作チケット・共有リソース調停（正本§9）:**
  - 機種SPI `PublicOpenOps::plan_stream` および `PublicStreamPlan` により宣言されたリソースキーに基づき、既存の操作チケット（`OpenSlot::OperationControl`）をストリームの open から close（drain完了）まで保持する。別状態機械は一切作成しない。
  - **同key調停と別key進行:** 同一の共有リソースキー（fixture では key 900）を要求する別受信部やカードの操作は、ストリーム操作チケットの保持中および close PENDING 中は内部で排他待機（調停・直列化）され、ストリーム close 完了後に進行する。一方、無関係なリソースキー（fixture では key 42）を要求する操作は、ストリーム reader が待機中であっても並行して進行する。
  - **caller直列条件と内部共有リソース調停の区別:**
    - 同一受信部に対する並行操作（例: start_stream と tune_by_frequency の同時呼出し）は、caller 側の正本 L3 §9.330「同じ受信部への操作は、一つのスレッドから順番に行う（直列）」という前提条件の外である。したがって、同一受信部に対する並行呼出しでコールバックが重なったとしても、許可利用条件における不具合とは扱わない。
    - 異なる受信部やカードであっても、同一の USB や内部共通リソースを共有する場合は、L3 §9.333 および採用設計 5eede671 §4 に従いライブラリ内部で排他調停を行う。
- **親子閉鎖寿命とPENDING再試行:**
  - 親enclosureは配下でオープンされた子ストリームを追跡する。親 close（`dtv_close_enclosure`）経路は、子ストリームに対してキャンセル通知とブロック待機条件変数の起床（`CANCELLED` 解除）を行い、子ストリームの停止・drain完了を待ってから親のクローズを進める。
  - 子ストリームの stop / drain が完了せず PENDING の間は、親 close も `DTV_CLOSE_PENDING` を返し、親資源（オープン状態・ランタイム等）は保持される。後続の閉鎖状態再試行で子ストリームが drain 完了した後に親の閉鎖が完了する。ストリームオープン処理中（opening中）も親が追跡し、処理完了を待ってから閉鎖を進める。
- **ライフサイクル・世代フェンス・失敗時クリーンアップ:**
  - ストリームハンドルは親enclosureの参照を保持し、ストリーム close 完了まで親enclosure openスロットとの依存を維持する。
  - **個別エラーコード変換と部分失敗回収:** 機種SPI（`plan_stream`, `prepare_stream`, `start_stream`）が返す `dtv_internal_result` は、公開変換関数 `public_error_code()` により個別の公開エラーコード（`DTV_ERROR_UNSUPPORTED`, `DTV_ERROR_NOT_READY`, `DTV_ERROR_INVALID_ARGUMENT`, `DTV_ERROR_USB_IO`, `DTV_ERROR_TIMEOUT`, `DTV_ERROR_RESOURCE_EXHAUSTED`, `DTV_ERROR_BUSY`, `DTV_ERROR_DISCONNECTED`, `DTV_ERROR_CANCELLED`, `DTV_ERROR_INTERNAL` 等）へ個別に変換される（特定エラーコード `USB_IO` への一律一般化ではない。fixture が `USB_IO` を注入検証したことと公開規則を区別する）。部分開始（partial-start）等の失敗時は、`producer_started` であれば `stop_stream`、`prepared` であれば `after_stream_drain` を呼び出し、状態を closed として待機者を起床させ、チケット解放および親enclosure参照の解放を行ってリソースを安全に回収する。
  - ストリームごとに一意の `stream_generation` をインクリメントし、以前の古いストリームに属する遅延ソースチャンクや無効な世代のチャンクはフェンスにより破棄され、新しいストリームに混入しない。
- **後続工程との境界:**
  - 本PR（PR44）で接続されたのは、公開ストリーム受信API（`dtv_open_stream`, `dtv_read_stream` 等）、有限188/192バイトTS変換配送、決定56に基づく合成連続ソースの受信部別有界キュー満杯・保存済み順序配送・`SLOW_CONSUMER` 終端接続、決定57に基づく公開PIDフィルタ機器設定・新着選別接続、共有IN継続動作、リソースチケット・共有リソース調停・世代フェンス、親子閉鎖寿命連携、およびストリーム独立ライフサイクルである。
  - 実機5製品固有のPID機器設定コマンド、TSストリーム転送・DMA/USBエンドポイント・start/stopコマンド/ハードウェア通知フェンス、カードAPDU送受信API、daemon IPC、CLI TSコマンド統合は本PRの対象外であり後続工程である。

## 公開カードリセットと公開ATR getter接続（PR45/PR47、合成reset・取得済みATR・決定51・寿命・直列化接続）

開いた筐体（`DTV_OP_CARD` 対応）に対してカードリセットを実行し、合成機器から得たATR（Answer To Reset）の完全性・TCKをCore共通で検査・所有して公開ポインタと長さを返却する公開C API関数（`dtv_reset_card`）、およびリセットで取得・保持されたATRを安全に読み出す公開C API関数（`dtv_get_card_atr`）、三層の機種ディスパッチ（Core共通層→製品層→機種層 `reset_card`）、同一筐体内カード操作の直列化、二重世代フェンス（接続エポック・リクエストエポック）、決定51に基づく挿抜世代・イベント不変性、L3 §6.2に基づくgetter-only停止回収、およびSharedInOwner並行動作を接続した。

- **公開カードリセットAPIと引数・能力検証:**
  - `dtv_reset_card(dtv_enclosure *object, const dtv_call_options *options, const std::uint8_t **out_atr, std::uint64_t *out_size, dtv_error *error)`:
    - 引数検証: `object == nullptr || out_atr == nullptr || out_size == nullptr`、または `options != nullptr && options->struct_size < sizeof(*options)` の場合は即座に `DTV_ERROR_INVALID_ARGUMENT` を返す。初期化として `*out_atr = nullptr; *out_size = 0;` を設定する。
    - 能力照会: `dtv_get_enclosure_support(object, DTV_OP_CARD, ...)` を行い、対応能力が `DTV_YES` でない場合（`DTV_NO` または `DTV_UNKNOWN`）は、ハードウェア I/O や機種ディスパッチを行わず直ちに `DTV_ERROR_UNSUPPORTED` を返す。
- **公開ATR getter API（`dtv_get_card_atr`）と引数・能力検証:**
  - `dtv_get_card_atr(dtv_enclosure *object, const dtv_call_options *options, const std::uint8_t **out_data, std::uint64_t *out_size, dtv_error *error)`:
    - 引数検証と初期化: `object == nullptr || out_data == nullptr || out_size == nullptr`、または `options != nullptr && options->struct_size < sizeof(*options)` の場合は即座に `DTV_ERROR_INVALID_ARGUMENT` を返す。出力ポインタが存在する場合は初期化として `*out_data = nullptr; *out_size = 0;` を設定する。
    - 能力照会: `dtv_get_enclosure_support(object, DTV_OP_CARD, ...)` を行い、対応能力が `DTV_YES` でない場合（`DTV_NO` または `DTV_UNKNOWN`）は、ハードウェア I/O や機種ディスパッチを行わず直ちに `DTV_ERROR_UNSUPPORTED` を返す。
    - オープン状態検証: 親スロットが存在し、`slot->phase == Phase::open` でなければ `DTV_ERROR_NOT_READY` を返す。
  - **暗黙リセットなし・新I/Oなし・再解析なし:**
    - `dtv_get_card_atr` は以前の `dtv_reset_card` 等によって実取得され、Core の `EnclosureRegistry` に保存されている有効な ATR を読み出す getter である。
    - 暗黙の reset、新たな USB I/O、製品ディスパッチ、ATR 再解析は一切行わない。
  - **内部直列化・操作チケット・キャンセル連携:**
    - 公開操作チケット（`acquire_public_card_ticket`）によりスロットのキャンセル・クローズ（`cancel/close`）ゲートと連携。
    - 筐体ごとの `CardFunctionLease`（`std::timed_mutex`）を非同期ループ内で `try_begin_card_function`（最大 2ms try_lock）により取得してリセット操作等と直列化。キャンセル時は `DTV_ERROR_CANCELLED`、タイムアウト時は `DTV_ERROR_TIMEOUT` を返す。
    - 専用 worker スレッドは生成せず、待機中に `SharedInOwner` を保持しない。
  - **現在値失効と返却 byte 領域寿命の分離（正本 §6.7）:**
    - エポック照合（`card_atr_connection_epoch` と `card_atr_request_epoch`）により現在値の有効性を確認。
    - カード抜き差し（挿抜通知）が発生すると内部リクエスト世代（`card_request_epoch`）が進むため、現在値としては失効し後続の getter は未取得（`DTV_ERROR_NOT_READY` / `DTV_ERROR_NO_CARD` / `CARD_REMOVED`）を返す。
    - 一方で、すでに呼び出し側へ返却された byte 列領域（`card_atr`）自体は、挿抜通知ではクリア・破壊されない。
    - **寿命境界**: 正本 §6.7「次にその筐体のカードの関数を呼ぶまで有効」。次の同一筐体に対するカード操作（`dtv_reset_card` 等の `begin_card_reset`）が寿命境界となる。
    - **別 receiver との並行性**: 別 receiver の操作（選局、188バイトストリーム読取、signal等）では寿命は一切壊されず、返却ポインタの内容は完全に有効を保つ。
  - **エラーマッピングと READY 通知の写像:**
    - ATR 未取得時は `DTV_ERROR_NOT_READY`。
    - カード不在（`DTV_CARD_ABSENT`）時は、挿抜世代 `card_generation == 0` なら `DTV_ERROR_NO_CARD`、`card_generation > 0` なら `DTV_ERROR_CARD_REMOVED`。
    - ATR 実体を伴わない `READY` 通知を受信した場合、公開上 `DTV_CARD_PRESENT` へ写像される（正本「`DTV_CARD_READY` は ATR 取得済み」の定義に準拠）。ただし、この写像は Core 内部の実装材料であり、実製品での検証済み動作とは扱わない。
- **直列化・二重世代フェンス・状態遷移:**
  - **直列化（Lease）:** 筐体ごとに設けられた `card_function_mutex`（`CardFunctionLease`）を取得し、同一筐体に対するカード操作（`dtv_reset_card` / `dtv_get_card_atr` 等）を内部で厳密に直列化する。
  - **開始時初期化とエポック更新:** `begin_card_reset()` において、前回の ATR 保持領域（`card_atr`）をクリアし、カード状態を `DTV_CARD_UNKNOWN` に設定し、内部リクエスト世代（`card_request_epoch`）をインクリメントする。
  - **二重世代フェンス:** 機種からの応答完了時（`finish_card_reset()`）に、`card_reader_connection`（USB/リーダー接続世代）および `card_request_epoch`（リクエスト世代）の双方が開始時と一致しているかを照合する。切断や旧リクエストに対する遅延応答はフェンスにより安全に排除（`DISCONNECTED`）される。
- **決定51との整合（挿抜世代・イベント不変性）:**
  - カードリセットはカードリーダーに対する制御操作であり、物理的なカードの抜き差しではない。
  - リセットの成功、失敗、取消、および close PENDING において、ライブラリが観測したカード抜き差し世代（`dtv_get_card_generation()`）を進めず、`CARD_INSERTED` / `CARD_REMOVED` イベントも発生させない（正本 §4.4, §8, 決定51）。なお、代表fixtureで世代が 0 であるのは初期fixture条件（オープン直後の初期観測状態から抜き差ししていない）に基づくものであり、一般的なリセット操作で世代を 0 へ初期化・リセットする一般規則ではない。
- **Core共通のATR構造・バイト長・TCK検査:**
  - 機種層から返された生 ATR byte 列に対し、Core 共通の `valid_card_atr` による実装レベルの構造・完全性検査を実施する（本検査は実装コードで確認したバイト列構造・長さ・チェックサムの検査であり、ISO/IEC 7816-3 規格全体の完全適合試験やプロトコル・タイミング等の適合保証ではない）。
    - 最小 2 バイト（TS, T0）から最大 33 バイトの範囲内であることを確認。
    - TS byte が直接規約（Direct convention: `0x3B`）または逆規約（Inverse convention: `0x3F`）であることを検証。
    - T0 byte の下位 4 ビット（historical bytes 数: 0〜15）および上位 4 ビット（interface bytes: TA1〜TD1 の存在フラグ）をデコード。
    - Interface bytes を順次解析（各グループ最大 16 回）。TDx byte のプロトコル番号 T != 0（T=1 等）が存在する場合、TCK チェックサム必須フラグ（`requires_checksum`）を立てる。
    - Historical bytes の長さ境界を確認。
    - `requires_checksum` が真の場合、T0 から TCK までの全 byte の XOR チェックサムを計算し、結果が 0 であることを検証。
    - 解析消費バイト数 `offset` が入力 byte 列の長さ `atr.size()` と完全に一致することを検証（途中切れ、末尾の余剰ゴミバイトを排除）。
  - 不正な ATR、途中切れ、TCK 不一致の場合は `DTV_ERROR_CARD_IO` を返却し、カード状態を `DTV_CARD_READY` へ遷移させず、`out_atr = nullptr`, `out_size = 0` を維持する。
- **返却領域の所有権と寿命（正本 §6.7）:**
  - 検査に合格した妥当な ATR は、Core 内部の `EnclosureRegistry::State::Root::card_atr`（`std::vector<std::uint8_t>`）にコピー保存される。
  - 公開ポインタ `*out_atr`（および `dtv_get_card_atr` の `*out_data`）はこの Core 所有領域のアドレスを指し、`*out_size` にそのバイト長を返す。
  - **ポインタの寿命:** 正本 §6.7 の規定どおり「次にその筐体のカードの関数を呼ぶまで有効」である。次の同筐体カード関数が開始され `begin_card_reset()` で領域がクリアされるまで有効性を維持する。
  - **別 receiver との並行性:** 別 receiver の操作（選局、188バイトストリーム読取、signal等）では `card_function_mutex` や `card_atr` は一切参照・変更されないため、返却ポインタの内容は失効せず完全に有効を保つ。
- **SharedInOwner との並行進行:**
  - カードリセット要求は `run_public_card_operation` を経由して `SharedInOwner` のチケット機構で発行される。
  - 同期 USB 待機（`wait_for_reply`）は呼び出し元スレッド（caller）側で行われるため、`SharedInOwner` の worker スレッドはブロックされない。
  - これにより、リセット ACK 待機中であっても、同一エンドポイント上のカード状態通知（`CARD_PRESENT` 等の反映）や、別 receiver の TS 配送（192バイトから188バイトへの変換読取）が並行して妨げられずに進行する。
- **L3 §6.2 に基づく PENDING close の state getter からの再開・回収:**
  - `dtv_close_enclosure()` は操作チケット待機中（呼び出し元スレッドの処理中等）にタイムアウトするとスロットを `DTV_CLOSE_PENDING` として一旦戻る。このとき後続のランタイム停止処理（`runtime->stop()`）が未実行のまま残る場合がある。
  - 利用者が正本 L3 §6.2 の契約に従い `dtv_get_enclosure_close_state()` でポーリングを行う際、従来はランタイムの worker 終了を待つだけで未実行のクローズ処理を再開しなかったため、スレッド終了待ちで PENDING のまま停止する問題があった。
  - これを解消するため、getter 側においてスロットが `PENDING` の場合、`slot->mutex` を保持しない状態で既存の `dtv_close_enclosure` 継続処理を 1ms タイムアウトで呼び出し、中断していたクローズパイプライン（ランタイム停止・キャンセル・drain・join）を安全に再開・進行させるよう修正した。
  - 利用者は正本通りの getter-only（`dtv_get_enclosure_close_state()` のみ）を維持し、追加の worker スレッドや別状態機械、新 API を導入することなく、安全に `DTV_CLOSE_CLOSED` への回収が完了する。
- **型付き私有カード搬送境界（PR48、C10初段・byte/chunkと完成T=1 blockの型分離・実往復・結果確定性・無自動replay・close drain）:**
  - **位置付けと私有境界:**
    - 公開APDU API（`dtv_transmit_apdu`）や共通T=1プロトコル（I/R/S block, sequence, WTX, chaining, EDC等）、PC/SC、実製品搬送の実装前に必要な、カード物理搬送の2形態を合成交換で確認できる最小の私有Core SPI単位（C10初段）である。
    - 公開C API宣言や公開APDU接続は変更しておらず、公開APDUの完成と取り違えない。
    - 機種ファームウェアの物理frameとCore共通プロトコルを混同しない。物理chunk長、転送速度、上限、retry回数を新しい共通仕様にしない。
  - **2つの搬送形態の型による厳密な分離:**
    - `dtv::spi::CardByteWrite`（`std::span<const unsigned char> bytes`）および `dtv::spi::CardByteRead`（`std::size_t capacity`）: 機種のUART/mailbox等を低位のwrite/read byteまたはchunkとして呼び出し、進行byte数を扱う型。raw byte/chunkの組立やT=1 block化はこの層では行わない。
    - `dtv::spi::CardT1BlockExchange`（`std::span<const unsigned char> block`, `std::size_t response_capacity`）: 機種FWが既にframe処理を行い、完成したT=1 block全体（NAD/PCB/LEN/INF/LRC等）を一回のexchangeとして送受信する型。Coreが機種FWのframe検査を二重に行わない。
    - これらを `std::variant<CardByteWrite, CardByteRead, CardT1BlockExchange>`（`CardTransportOperation`）として型で区別し、単一の「送受信byte列」へ押し潰さない。
  - **Core所有バッファ、送受信進行、応答完了、結果既知性（確定性）:**
    - Coreが入力バッファの複製・所有を行い、応答受領用のバッファ（`std::vector<unsigned char>`）もCoreが所有して機種callbackには借用spanのみを渡す。
    - `CardIoOutcome`:
      - `tx_bytes_done`: 送信完了byte数
      - `rx_bytes_done`: 受信完了byte数
      - `reply_complete`: 応答が完全に受領されたか否か
      - `result_known`: 結果が確定（既知）しているか否か
      - `transport_error`: 搬送内部エラーコード
    - 送信前失敗、部分送信進行（partial tx）、完全応答、送信済みだが応答未完/欠落（timeoutやcancel、close時）を厳密に区別。
    - 出力を受け取れなかっただけの状態を「未送信」へ戻さず、結果不明（`result_known == false`）のまま呼出元へ返す。
  - **無自動replay（搬送層での自動再送排除）:**
    - 送信済みかつ結果不明の場合に、搬送層で勝手に同じ要求を再送（replay/retry）しない。
    - カード側で既に処理が進んでいる可能性があり、勝手な再送は状態破壊や誤二重実行を招くため。通常のT=1 block retryは後続の共通プロトコル層の責務とする。
  - **既存リソース・機構の再利用とclose drain:**
    - 既存の操作チケット（`acquire_public_card_ticket`）、筐体ごとの `CardFunctionLease`（`std::timed_mutex`）、`SharedInOwner`（制御応答配送・demux）、USB output runtime、単調期限、cancel/close制御をそのまま再利用。新workerスレッドは生成しない。
    - **エポック照合:** submit前（開始時）に `card_state == DTV_CARD_READY` かつ現ATRと一致する reader pointer、`connection_epoch`、`request_epoch` を取得し、operation ticket取得後に `CardFunctionLease` を得て再照合。reply受領後にも再照合し、古いepochの遅延応答を呼出元へ返さない。
    - **close/cancel時のdrain:** timeout/cancel経路においても完了済み送信completionを先に回収（drain）し、tx進行量とretired状態を記録してからcallerが抜ける。これにより遅延close競合時にも確認済み送信進行量を正しく保持する。
- **後続工程との境界:**
  - 本PR（PR48）で接続されたのは、公開カードリセット（`dtv_reset_card`）、取得済み ATR の公開 getter（`dtv_get_card_atr`）、三層機種dispatch、合成reset要求・応答、Core共通ATR構造/TCK検査、返却領域所有・寿命、直列化・二重世代フェンス、決定51整合、L3 §6.2 に基づく PENDING close 停止回収、および型付き私有カード搬送境界（C10初段: byte/chunkと完成T=1 blockの型分離、合成機器との実往復、Core所有バッファ、送受信進行・結果確定性・無自動replay・close drain）である。
  - Core共通T=1 protocol（I/R/S block, sequence, WTX, chaining, retry）、公開APDU送受信API（`dtv_transmit_apdu`）、PC/SC、実機5製品固有の電源/reset/card transport/コマンド、実USB機器、Android ランタイム実機動作は本PRの対象外であり後続工程である。今回公開APDU完成や全カード機能完成とはしない。

## CLI接続・既存レガシーとの境界・未実装範囲

- **CLI接続:** 生成された main は、argc/argv を変更せずそのまま既存Coreの `dtv_cli_main` へ渡す。L1の全parserや全subcommandの実装が完了したわけではない。
- **レガシーとの境界:** 既存のメタデータAPI（`dtv_model_count`, `dtv_model_get`）や旧内部ランタイム（USB、discovery、firmware転送）は残存しているが、本PRで導入された新L3 registryとは分離されている。正本との識別子衝突を回避するため、内部レガシー型・状態名は機械的に改名されている。また、旧来の `UsbContext` 既定コンストラクタによる2-worker常駐経路もレガシー境界として残存している。
- **未実装・未接続範囲:**
  - 合成機種における完全rootの公開 open / close / state / release / cancel、1-worker ランタイム、共有 IN 継続受信、有界返信配送、claim 排他予約、共通操作受付・物理scope ticket予約、合成共有INからの公開CARD状態・CARD_INSERTED/REMOVEDイベント接続、安定した公開receiver配列・getter/support・動的宣言確定接点、公開LOCK_ACQUIRED/LOCK_LOSTイベント接続、公開シグナル取得API（`dtv_get_signal` によるロック現在値照会・決定53 reason同期）、公開周波数選局API（`dtv_tune_by_frequency`、合成機種SPI、決定54選局時ロック状態遷移・世代フェンス）、公開チャンネル選局API（`dtv_tune_by_channel`、厳密ASCII文法・決定55整数Hz換算/衛星IF・セレクタ既定/排他）、公開ストリーム受信API（`dtv_open_stream`, `dtv_read_stream`, `dtv_cancel_stream`, `dtv_close_stream`, `dtv_get_stream_close_state`, `dtv_release_stream`, `dtv_get_dropped_bytes`, `dtv_get_desyncs`, `dtv_set_pid_filter`）、有限188/192バイトTS変換配送（Core TsFramer・製品transform接続）、合成連続ソースにおける決定56連続TSキュー満杯・SLOW_CONSUMER終端接続、決定57に基づく公開PIDフィルタ機器設定・新着選別接続、公開カードリセットAPI（`dtv_reset_card`、合成reset、Core共通ATR検査・返却領域所有、直列化・二重世代フェンス、決定51整合、L3 §6.2 getter-only停止回収）、公開ATR getter API（`dtv_get_card_atr`、取得済みATR読み出し、現在値失効と返却領域寿命の分離）、および型付き私有カード搬送境界（PR48、C10初段: byte/chunkおよび完成T=1 blockの型分離・実往復・Core所有バッファ・結果確定性・無自動replay・close drain）は本PRで実資源へ接続完了。
  - Core共通T=1 protocol（I/R/S block, sequence, WTX, chaining, retry）、公開カードAPDU送受信API（`dtv_transmit_apdu` 等）、PC/SC、および実機校正CNR/電力測定の実装接続。
  - 実機5製品（PX4, PIX, FS1等）の製品固有open計画・固有identify・初回/継続共有INの実機パラメータ・分類・実機CARD/LOCK通知値・実機受信部宣言・実機ロック判定閾値・実機tuneコマンド/機器丸め/RF変換・実機ストリーム転送/start-stopコマンド/ハードウェア通知フェンス・実機固有PID機器設定コマンド・実機固有電源/reset transport/コマンド。
  - ファームウェア転送 API。
  - daemon / wire API、各製品リポジトリの実装接続。
  - CLI channel / ts / card 実行の完成。
  - 空成功 stub のような不完全な実装は行わない。
- **決定事項:** 正本における決定11（イベント6種）、決定18（固定スロット容量）、決定26（公開関数の分類：カード低位SPIとCore共通ATR・T=1・APDU）、決定47（一覧の扱いと順序）、決定48（slot分類）、決定49（runtime slug）、決定50（sticky RESOURCE_LIMIT）、決定51（カードの最初の状態：初回観測イベントなし・generation 0維持、リセットでの不変性）、決定52（受信のロックの最初の状態）、決定53（測定値の理由 enum・マッピング）、決定54（選局し直したときのロック：選局作用時のUNKNOWN/TUNE_CHANGED、最後既知イベント基準の保持、選局毎初回リセットなし、旧観測フェンス）、決定55（チャンネルから周波数への換算：整数Hz式、衛星IF、機器丸め/RF変換の製品責任、L1 §10セレクタ既定と排他の公開C適用）、決定56（読む側が遅くTS溜め場所が一杯になったときの扱い：保存済み188パケット返却後SLOW_CONSUMER、ストリーム終端、明示close再open、queue破棄はdropped_bytes非計上、USB受信継続）、決定57（PIDフィルタの決まり：初期全通過、count 0解除、非空置換、設定成功後の新着TSから選別、保存済みパケット内容・順序保持、除外パケットのdropped_bytes非計上、機種による機器設定）、決定58（PIDの範囲：0〜0x1FFF、範囲外は即時INVALID_ARGUMENT）は決定済みであり、未決事項を独断で仕様変更しない。

## 検証状況

- **実施済み検証:**
  - Coreルート構成からの fresh configure および全ターゲットの build 成功。
  - 対象全24件のテスト通過（CTest 24/24 pass、CLI contract、`usb_claim_lease_contract` 5回反復 pass を含む）。
  - 公開C consumer（`tests/fixture/card-events-public.c`, `discovery-public.c`）および関連fixture（`usb_claim_lease_contract`, `discovery_public_contract`, `discovery_registry_contract`）による検証通過：
    - 未オープン親enclosure拒否、187バイトcapacity不足拒否、先頭0x47の188バイトパケット受信と有限ソースEOF終端0、有限192/4ソースの分割17バイトcompletion・先頭3バイトノイズ・不正ユニット再同期破棄・dropped_bytes 3→195/desyncs 0→1・プレフィックス4バイト非計上・元188バイトpayload完全一致、188/0ダイレクトコピー、合成連続188/0ソースでの有界キュー満杯・保存済み5577パケット順序配送・drain後読取でのSLOW_CONSUMER=22終端・dropped/desyncs非計上・満杯後共有IN制御/カード通知完了・別受信部192/4並行配送。
    - 公開PIDフィルタ設定のYES/NO判定・非対応UNSUPPORTED（I/Oなし）・対応モデルCore/製品/機種dispatch機器設定ACK・機器側新着選別・初期全通過・非空置換・count 0解除・設定前保存済みパケット順序内容保持・除外パケット非framing計上・設定中別受信部192/4並行配送とカード通知完了・再open全通過初期化・旧世代排除。
    - 公開カードリセットのYES/NO判定・非対応UNSUPPORTED（I/Oなし）・対応モデルCore/製品/機種dispatch機器設定ACK・合成scanfixtureでの妥当ATR（`3B 80 01 81`）、NO_CARD、途中切れ、TCK不正、旧接続世代応答の拒否確認。
    - 公開ATR全byte・長さ、`CARD_READY` 遷移、世代不変（gen 0）、別receiver signal / 188バイトストリーム読取後のATRポインタ有効性維持、失敗後のポインタ・状態初期化、cancel、close PENDINGライフサイクル。
    - ブロック読取待機タイムアウト、cancel/closeによる読取ブロック待機解除、親enclosure closeによるブロック読取待機CANCELLED解除、親子PENDING・再試行・drain完了順序、共有リソースkey競合調停と別key並行進行、任意旧世代および直前ストリーム世代チャンクのフェンス抑止、他receiver/card並行動作、prepare/start失敗時の個別エラー返却と部分開始時の停止・drain回収、close PENDING/retry/releaseライフサイクル、既存共有IN先行配送とproducer開始順序、親enclosureとストリームの独立性等。
  - 追加 fixture による代表実測:
    - reset ACK 保留中における `CARD_PRESENT` 公開反映・初回イベントなし / gen 0（決定51）。
    - 別 receiver 1（192バイト source → 188バイト読取）の並行進行完了。
    - 最後の ACK 配送による妥当 ATR 返却・`CARD_READY`・gen 0 遷移を確認。
  - Android NDK r27c (arm64-v8a, API24) 環境での `dtv-userland` 静的ターゲットビルド成功。
  - `git diff --check` 通過。
  - CI・fixture切り分け履歴:
    - TS 同期確立経緯: 初回は 192バイト source を 1 TS 単位だけ注入したため同期 framer が 188バイト変換同期を確立できず公開 read が timeout。5個の 192バイト TS 単位を供給するよう fixture を修正し同期確立・読取成功（既存期待値の変更・skipなし）。
    - close PENDING の getter-only 停止回収修正の経緯:
      - 旧 head `25b645a25159358eb60c12b2743347c86aadb5c9` の Windows push CI（run 43595306769）で失敗した際、試行として caller 終了同期後に fixture から `dtv_close_enclosure` を再度呼び出す形（commit `65cfd397`）を一度導入した。しかし独立 QA の照合により、PENDING 中に再度 close を呼び出すことは L3 §6.2（クローズ処理中は利用者は `dtv_get_enclosure_close_state()` で状態確認のみを行い、再度クローズを呼んではならない）に違反することが確認された。
      - 次に fixture 側を getter-only ポーリングへ修正（commit `6849f77`）したが、事前検証用公開 snapshot `0480927d`（push 43602290254, PR 43602294503）の Windows CI において、約 2.2 秒後に `pending reset=21 error=21 close=2` でタイムアウト失敗（22/23 pass）することが判明した。
      - 原因調査の結果、初回 `dtv_close_enclosure` が操作チケット待機タイムアウトで PENDING へ戻ると、未実行のランタイム停止（`runtime->stop()`）を既存 getter が再開せず worker 終了待ちだけになり、worker が停止せず PENDING に留まる制御フロー欠陥が特定された。
      - 最新 head（`42ab774861cb21b3376d8cbfc6b524c2ca4b2f19`）において、getter 側から mutex を保持せず 1ms タイムアウトで既存 close 継続処理を呼び出して未実行のランタイム停止を再開・完了させる修正（+10, -4 行）を実施。利用者は getter-only のまま、追加 worker や新 API、fixture 期待値変更なしで正常に回収されるようになった（Linux 全24 CTest、`usb_claim_lease_contract` 5回反復 pass、CLI help 両 entry point pass、NDK r27c static build pass、whitespace error なし）。
      - なお、古い旧 head における push 失敗 / PR 成功の差異の正確なスケジューリング根本原因は未確定を維持する。
    - 公開 CI 検証および最新運用の記録:
      - 最新方針として、CI は `Khronos31/quiet-shelf`（公開リポジトリの完全一致検証ブランチ、PR 1）において同一内容を検証し、private リポジトリ側の請求設定（spending limit）を変更しない運用方針となった。
      - 公開成功 snapshot `71f248b76c4bcd68cf7cd6462781665479fbd232` は、Core head `42ab7748` と 87 path/mode/blob が完全に一致し、GitHub Actions CI の push run `43604490188` および PR run `43604494561` において Linux 24/24 pass、Windows 23/23 pass（`usb_claim_lease_contract` 含む）、CLI contract pass をすべて達成した。
      - 旧失敗 snapshot `0480927d`（push 43602290254, PR 43602294503）の失敗記録、旧課金制限による非開始 run（push 43600730239, PR 43600733981）、および旧 head `5481750` の CI 成功（push 43596896675, PR 43596903035）は過去の経緯としてそのまま記録を残す。
    - PR48（カード搬送）fixture切り分けおよび修正経緯:
      - 初回fixture応答長誤設定の修正: byte-write synthetic demux が 1 byte の応答領域に対して 4 byte を通知し、waiter から `RESOURCE_EXHAUSTED` となった。byte-write 応答長を 1 byte へ合わせて解消（既存期待値の弱化・skipなし）。
      - close/cancel 時の完了済み送信 completion 回収不足の実装修正: close/cancel 直後に runtime が完了済み USB 送信 completion を回収する前に caller が抜け、送信進行量が 0 のままだった（Core の cancel 経路で完了通知を先に回収しない実装不足）。typed transport の timeout/cancel 経路で完了済み completion を回収し、tx 進行量と retired 状態を記録するよう修正して解消。これらは実機ハードウェアでの検証済みではなく、合成環境・fixture での修正実績である。
  - PR47作者検証: 変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 pass（2.37秒。reset 前の getter NOT_READY、reset 後の getter バイト列・長さ完全一致、getter 単独での追加 endpoint 出力なし、reset 失敗後の出力クリアと getter NOT_READY、unsupported 筐体での出力クリア、別 receiver signal 進行後の pointer 有効性維持等を確認）、`git diff --check` 通過。
  - PR48作者検証: 変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 pass（2.47秒。既存CARD/receiver/close回帰に加え、byte write/readとcompleted T=1 block往復、送受信byte数、応答欠落時unknown/no-retry、in-flight closeのCANCELLEDとPENDING回収を確認）、全target build 成功（`cmake --build build --parallel 2`）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `e7f6694155cb90d1a0926736ded4d64f83f1ff5b`）。
- **未実施事項・過去の不合格履歴:**
  - 独立QA（コミット `4520f4e927c677653c937b337056694c1f668604`）において、PR41修正前 head（`dd9220bbe6aa6f6ed68bd41cd83b539343d22877`）に対する独立プローブにより、親 close 時にブロック待機中の stream reader が解除されない不具合（failure 30 / exit 91）が検出・不合格判定された履歴がある（PR41修正により解消、不合格の事実を改ざんしない）。
  - 今回の PR48 head（`e7f6694155cb90d1a0926736ded4d64f83f1ff5b`）に対する全 CTest suite、Android NDK r27c 静的ビルド、公開 quiet-shelf 最終 CI、および由来確認は後続 QA に委任されており、未実施・成功済みにしない。本 PR48 は PR47 を base とするスタック PR（未 merge）である。
  - stale connection/request epoch の遅延応答専用 fixture での検証（旧応答が後続 session の waiter へ入らないことの確認）および typed transport 実行中に別 receiver read と CARD 通知が進行する並行組み合わせ専用 fixture での検証は後続 QA として区別し、成功済みにしない。公開 APDU 完成の断定は行わない。
  - 実USB上の物理リソース衝突・物理unplug時の実機DISCONNECTED挙動、実機5製品の固有PID機器設定コマンド・実機TS転送接続・実機固有電源/reset transport/コマンド・実機カードAPDU・Androidランタイム実機動作・性能保証・全製品完成は未測定・未検証。
  - 実機5製品のlock観測source/byte/閾値・校正CNR電力測定・実機tuneコマンド・実機ストリーム転送・機器丸め/RF変換は未接続・未設定（実値・性能SLAの推測なし）。card APDU、daemon IPC、CLI channel/ts/card 実行も未接続。
  - 実機 Android 端末・USB 機器での実機動作確認、実機切断、permission 受渡し動作確認は未実施。架空の実機成功や全 OS 保証は行わない。
  - 実機5製品の初期化・実機エンドポイント値・実機CARD/LOCK通知値・カードAPDU・公開操作接続・同期途中中断保証や数値 SLA は保証しない。
