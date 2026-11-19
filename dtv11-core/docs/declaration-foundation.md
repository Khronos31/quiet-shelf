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
- **共通T=1完成block交換口（PR49、C10後段/C11入口・ATR由来条件・codec・byte累積と完成block FW責務分離・共通結果/進行・無replay）:**
  - **位置付けと私有境界:**
    - PR48で成立した2形態の私有搬送（byte/chunk搬送および完成block搬送）を、後続のCore共通T=1状態機械が消費できる単一の完成block交換口（`exchange_public_t1_block`）へ接続した私有Core単位（C10後段/C11入口）である。
    - 公開C API宣言や公開APDU接続は変更しておらず、公開APDU完成やT=1規格全体準拠・実製品搬送完了と取り違えない。
    - 機種FWの物理frameとCore protocolを混同せず、CRC/LRCやIFSC/IFSD数値を新公開上限にしない。
  - **ATR由来のT=1 session条件:**
    - ATRのインターフェースバイト解析（`card_atr_t1_parameters`）により、選択プロトコル（T=1選択）、TA2 specific mode、TC(T=1)によるCRC選択（0: LRC, 1: CRC）、TA(T=1)によるIFSC（カード受信情報長、規定初期値32）、IFSD（端末受信情報長、規定初期値32）を決定し、セッション条件として保持。
  - **Core T=1 block codec:**
    - T=1 block構造（NAD, PCB, LEN, INF, EDC）を一度だけ組み立て・解析。
    - LRC: NADからINF末尾までの全バイトXOR（1バイト）。
    - CRC: CRC-16/X-25系（反射多項式 0x8408、初期値 0xFFFF、補数、low byte先行）（2バイト）。
    - INF長は選択されたIFSC内に制限し、応答INF長はIFSD内に制限。
  - **2つのアダプタ経路の責務分離:**
    - **Byte/chunk adapter (`CardTransportMode::byte_chunks`):**
      - 同一の既存card operation ticket（`retained_ticket`）およびリソースキーを維持・再利用して直列化。
      - 要求block全体を `CardByteWrite` で送信（部分write進行を累積）。
      - 応答blockを `CardByteRead` で累積読取（まずヘッダ3バイトを読み、続いて `LEN + EDC` バイトを読み、さらに1バイト余剰readで完全block終了を検証）。
      - Core自身がEDCを計算・照合してframe境界と整合性を確定し、不完全な途中バイトを完成blockとして上位へ返さない。
    - **Complete-block adapter (`CardTransportMode::complete_block`):**
      - Coreが生成した完成block（NAD+PCB+LEN+INF+EDC）を一度だけ `CardT1BlockExchange` へ渡す。
      - 機種FWが保証する物理frame/EDC検査をCoreが二重に重複実装しない。
      - Core側では安全な構造、長さ、NAD整合、LEN <= IFSDを確認して上位共通処理へ渡す。
  - **共通結果・所有・進行・無replay:**
    - 両アダプタの応答を共通の内部構造 `T1BlockResult`（nad, pcb, inf, raw_block_length, io: `CardIoOutcome`）へ正規化。
    - 要求・結果バッファはCoreが所有し、借用spanは呼出し中のみ有効。
    - `CardIoOutcome` により送受信バイト進行、reply_complete、result_known、transport_errorを保持。
    - 送信済みかつ結果不明（timeout, cancel, close時等）の場合に、搬送層で同一要求を自動再送（replay）しない。通常のT=1 block retryは後続の共通プロトコル層の責務とする。
  - **代表fixtureにおける並行進行・遅延旧epochフェンス・readiness条件（C7/C8実測事実）:**
    - **保留中の並行通知・別receiver読取:** case18 fixtureにおいて、CRC選択済みセッションのcomplete-block T=1交換を保留中に、別エンドポイントから公開 `CARD_REMOVED` 通知を配送。公開 `dtv_get_card_state` が `ABSENT`、`CARD_REMOVED` イベント発行、`card_generation` 増加を確認した時点でもT=1 callerは保留中（pending）のまま維持される。
    - **別receiver readとの因果同期:** 別 receiver 1 の live stream generation を使い 192バイト source パケットを endpoint 0x82 から配送し、公開 `dtv_read_stream` が 188バイトを返すことを因果同期で確認。read 完了時も CARD state は ABSENT で T=1 caller は保留中（pending）を維持。
    - **遅延旧epoch応答フェンスと進行量:** CARD_REMOVED 後、元の T=1 request transaction へ CRC-valid な遅延 reply（INF 6F00）を配送した際、caller に block/INF を返さず、`raw_block_length=0`、INF empty、`tx_bytes_done=8`、`rx_bytes_done=0`、`reply_complete=false`、`result_known=false`、追加 OUT なし（OUT 1回のみ）を確認。
    - **ABSENT readiness 条件とエラーマッピング:** 抜去による ABSENT 状態での実測内部結果は `NOT_READY`。`card_transport_context` は `card_state` が `READY` でない場合に `NOT_READY` を返し、request epoch 照合より先に return するため、公開エラーコードは `DTV_ERROR_NOT_READY` となる。これを純粋な接続・要求エポック不一致時の `DISCONNECTED` と混同せず、既存の readiness 条件に沿って確認（新しいエラー優先順位の規定や全物理競合の保証ではない）。
    - **新セッション確立と無自動replay:** `CARD_INSERTED` を公開 state/event で確認後に reset を行い、新 session で別 T=1 block を送信した際、応答 INF 9000 が正常に得られ、旧 6F00 が混入しないこと、成功と進行バイト数を確認。旧交換の OUT 送信は1回のみで、搬送層での自動再送や APDU replay は行われない。
- **Core内部単一I-block APDU交換とsession sequence（PR50、C11初段・Core内部APDU往復・送受N(S)管理・両adapter・IFSC検証・無replay・reset回復）:**
  - **位置付けと私有境界:**
    - PR49の共通完成T=1 block交換口（`exchange_public_t1_block`）を利用し、Core内部で1 blockに収まるAPDU要求・応答のI-block往復およびsession sequence状態遷移を接続した私有Core単位（C11初段）である。
    - 私有関数名に `public`（`exchange_public_t1_apdu`）が含まれていても、公開C API（`dtv_transmit_apdu`）の接続完了とは言わない。公開C宣言や製品SPIは変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。
    - R-block / S-block処理、WTX、chaining（連鎖ブロック）、規格内block retry、公開APDU、PC/SCは後続工程である。今回の単一block到達範囲（IFSC/254バイト）を公開最大APDU長としない。
  - **CardT1Sessionと送受sequence管理:**
    - reset成功（`finish_card_reset`）時にATR選択結果、reader connection epoch、request epochをsessionへ結びつけ、送受双方のsequence `N(S)` を 0 に初期化。
    - `card_t1_mutex`（`CardT1SessionLease`）により交換とリセットを内部で直列化。
    - `begin_card_t1_session` でエポック照合および現在の送受sequenceを取得し、有効な応答ブロックを受理・確定したときのみ `finish_card_t1_exchange` で送受sequenceを交互にトグル更新（0→1→0...）。送信前拒否ではsequenceを進めない。
  - **単一I-block APDU交換器（`exchange_public_t1_apdu`）の検証とエラー処理:**
    - **初期単一I-block時の入力検査とcommand chainingへの発展:** 当初は単一I-block APDU交換としてコマンド長が選択IFSC長（または254バイト）を超える場合に事前拒否していたが、後述のPR56（command chaining）によりIFSC超のコマンドはIFSC以下の片へ自動分割送信されるよう発展した。空のコマンド（長さ0）は引き続きI/O前に `DTV_INTERNAL_RESULT_INVALID_ARGUMENT` で事前拒否される（送信は行わず、進行量0、sequenceも保持）。なお、恣意的な公開command長上限は設けない。
    - **I-block構築と交換:** 入力command byte列を1個のI-block（PCB: `send_sequence ? 0x40 : 0x00`、INF: command）として共通交換口へ渡す。
    - **応答ブロック検証と拒否:**
      - NAD != 0、予約ビット（PCB bit 5..1 != 0）、期待sequence不一致（PCB bit 7 != receive_sequence）を厳密に検査し、不一致時は `USB_IO` として拒否。
      - R-block / S-block（PCB bit 8 == 1）、連鎖I-block（PCB bit 6 == 1、Mビット）を受信した場合は現単位では未対応として `UNSUPPORTED` で明示失敗。
      - 応答INFの末尾ステータス（SW bytes: 90 00等）は応答byte列としてそのまま保持し、Coreで別の公開ステータスへ作り変えない。
    - **送信後不明session無効化とreset回復:**
      - 送信後の不明結果（timeout/cancel/close時や受信ブロック異常時）では成功INFを返さず、`invalidate_card_t1_session` によりsessionを無効化（`card_t1_session_valid = false`）。新resetが成功するまで後続交換を拒否し、搬送層で勝手に再送（replay）しない。
      - reset後に新session初期sequence（N(S)=0）から回復する。
  - **両adapterでの実測実績:**
    - 同一LRC sessionにおいて、byte_chunksとcomplete_blockの両adapterから異なる短いAPDU（command 1: `00 A4...` → response `11 22 90 00`、command 2: `00 CA...` → response `33 44 90 00`）の連続2回往復を実測確認し、送受双方のsequence（0→1）とcommand別応答INFを照合。
    - 共有通知（CARD_REMOVED/INSERTED）、別receiver 192→188 read、遅延旧応答拒否、およびclose PENDING/drain回収が本APDU経路でも整合して動作することを確認。
- **受信R-block再送と単一I-block再送制御（PR51、C11中段・妥当R要求による同一I frame再送・3送信上限・正常I応答時のみsequence更新・S(RESYNCH)未実装失効・無replay）:**
  - **位置付けと私有境界:**
    - PR50のCore内部単一I-block APDU交換（`exchange_public_t1_apdu`）を基底とし、受信した妥当なR-blockの要求に応じて保持していた同一I-block（N(S)/INF/EDC）を規格範囲内で再送する私有再送制御を追加したCore内部単位（C11中段）である。
    - 受信R-blockによる再送制御はCore内部の私有プロトコル処理であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言や製品SPIは変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。また、実装の私有送信上限（3送信上限等）を新公開契約にしない。
    - 送信側からのR-block生成、S-block（WTX/IFS/RESYNCH）処理、I-block連鎖（chaining）、公開APDU、PC/SCは後続工程である。
  - **妥当な受信R-block判定と同一I frame再送:**
    - **受信R-blockの妥当性検証:**
      - NAD == 0、PCB bits 8..7 == `10`（R-block）、bit 6 == `0`、bit 5 == `N(R)`、bits 4..1 が error indication（0: エラーなし、1: EDCエラー、2: その他のエラー。3等は予約/不正）、LEN == 0 かつ INF なしを厳密に検査。
      - 受信した `N(R)` が直前に送信したI-blockの `N(S)` と一致することを確認。
    - **同一フレーム再送（no-replay）:**
      - 上記の妥当なR-blockを満たす場合のみ、APDU全体を再入・再生成せず、保持している同一の `N(S)` / INF / EDC フレーム（pre-encoded frame）を同じ session / ticket を維持して再送（retransmit）。
      - 両adapter（`byte_chunks` / `complete_block`）で同一の操作チケットおよび caller deadline を共有して動作。
  - **送受sequence更新契機と送信上限・session失効:**
    - **正常I応答でのみsequence更新:**
      - R-block受信時や再送途中では送受sequenceを進めない。
      - 期待するsequence番号を持った正常なI応答ブロックを受信・検証した時点で初めて、送受双方のsequence `N(S)` を一度だけトグル更新（0→1→0...）する。
    - **3送信上限とS(RESYNCH)未実装失効:**
      - ISO/IEC 7816-3:2006 §11.6.3.2（rules 7.4.1/7.4.2）に従い、初回送信を含めて最大3回のI送信（初回1回 + 再送2回）まで許容。
      - 3回送信後もR-blockが継続した場合、規格上の回復手順である `S(RESYNCH request)` が未実装のため、成功INFを返さず `session` を無効化（失効）して終了（新resetで回復）。
    - **timeout・不完全転送での扱い:**
      - タイムアウト、キャンセル、応答欠損を勝手にR-block要求と解釈せず、再送を行わない。
      - 初回送信でのタイムアウトは初回I送信のみ記録。R受信後の再送途中でブロックが不完全・タイムアウトとなった場合は、そこまでの進行量（tx/rx bytes）を保持しつつ結果不明（`result_known=false`）を集約してsessionを無効化する。
  - **両adapterでの実測実績:**
    - byte_chunks と complete_block の両adapterにおいて、`I(0,0) -> R(0) -> 同一I(0,0) -> 正常I応答` の再送往復、および次APDUでの `I(1,0)` 遷移を確認。
    - complete_block において、sequence が `N(S)=1` の状態での `R(1) -> 同一I(1,0) -> 正常I応答` の再送と sequence wrap（1→0）を確認。
    - error indication 1（EDCエラー）での再送受理、不正R（error code 3、誤N(R)、LEN!=0、誤NAD、S/chaining）での失敗とsession失効、3回試行上限での停止、初回到達前欠損での再送なし、R受信後欠損での進行量保持とsession失効を確認。
    - 既存のCARD通知、別receiver 188 read、遅延旧応答拒否、reset回復、close取消/PENDING drainの回帰を同一fixture内で確認。
- **受信I系列不一致に対するCore送信R-blockと系列回復（PR52、C11中段・Core送信Rによるカード応答回復・同一ticket/deadline・重複INF不採用・3R上限失効・無replay）:**
  - **位置付けと私有境界:**
    - PR50/51のCore内部単一I-block APDU交換（`exchange_public_t1_apdu`）を基底とし、受信した完全なI-blockの系列が期待する受信系列と異なる（重複I応答など）場合に、Coreから規格に沿ったR-block（other error）を送信してカードから正しいI応答へ回復させる私有プロトコル処理を追加したCore内部単位（C11中段）である。
    - 受信R-blockに応じた同一I再送（PR51）と、本PR（PR52）のCore送信R-blockによるカード応答系列回復は明確に区別される。
    - 本回復処理はCore内部の私有処理であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言や製品SPIは変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。また、実装の私有送信上限（3送信上限等）を新公開契約にしない。
    - 不完全/構造不正ブロックやBWTタイムアウトからのR生成、S-block（WTX/IFS/RESYNCH）処理、I-block連鎖（chaining）、公開APDU、PC/SCは後続工程である。
  - **Core送信R-blockの生成と回復動作:**
    - **完全受信I-blockの系列検査と重複INFの除外:**
      - 受信した完全なI-blockのNAD/PCB/LEN/EDC構造検査後、期待するカード受信系列 `N(S)` と照合。
      - 系列が不一致（直前の系列を繰り返す重複I応答など）の場合、そのINFを採用せず返却バイト領域へ確定しない。
    - **Core送信R-blockの構築と送信:**
      - 期待する受信系列 `N(R)`（期待0ならPCB `0x82`、期待1ならPCB `0x92`、bits 4..1=0010: other error）を持つR-block（NAD=00、LEN=00、セッション選択EDC）を生成。LRCセッションにおける期待1のR-blockは `00 92 00 92`。
      - 同一のAPDU session lease、retained card ticket、caller deadlineの上でR-blockを送信。APDU command Iの再送やAPDU全体の再入は行わない。
    - **正しいI応答の受理と系列更新:**
      - カードから期待系列を持つ正しいI応答を受信したときのみ、そのINFを結果として採用・複製。
      - 正常I応答の確定時のみ、送受双方のsequence `N(S)` を一度だけトグル更新（0→1→0...）し、次のAPDUへ進む。
  - **3R上限失効とタイムアウト・session失効:**
    - **3R送信上限とS(RESYNCH)未実装失効:**
      - ISO/IEC 7816-3:2006 §11.6.3.2（rules 7.1/7.2/7.4.2）に従い、R送信後も系列不一致の完全I応答が継続する場合、同一Rを最大3回まで送信。
      - 3回送信後も不一致が継続した場合、規格上の回復手順である `S(RESYNCH request)` が未実装のため、成功INFを返さず `session` を無効化（失効）して終了（新resetで回復）。
    - **R後タイムアウト・不完全転送での扱い:**
      - R送信後の応答がタイムアウトや欠損となった場合、成功INFを返さず `result_known=false`（不明結果）とし、進行量（tx/rx bytes）を集約してsessionを無効化（APDU command Iは再送しない）。
  - **両adapterでの実測実績:**
    - byte_chunks と complete_block の両adapterにおいて、正常I/I交換で受信期待系列を1へ進めた後、第二APDUの `I(1,0)` に対する重複 `I(0,0)` を検出し、command Iは単発送信のまま `R(1, other error) = 00 92 00 92` を送信して正しい `I(1,0)` 応答（INF: `33 44 90 00`）へ回復すること、および重複INF（`DE AD 90 00`）が漏れず正しいINFのみ返却されることを確認。
    - 回復後の次APDUが期待どおり `I(0,0)` となり、1回の成功交換分だけsequenceが進むことを確認。
    - complete_block において、R後も重複Iが続くケースで同一Rを3回送信して成功INFなし・UNSUPPORTED・既知進行量でsessionが失効しreset後に再開できること、およびR送信後の応答欠損で100ms caller deadlineにてTIMEOUTとなりINFなし・`result_known=false` でsessionが失効することを確認。
    - 既存のCARD通知、別receiver 188 read、遅延旧応答拒否、reset回復、close取消/PENDING drainの回帰を同一fixture内で確認。
- **Core私有BGT契約と送信前ガード（PR53、C11中段・採用BGT案A・三状態capability・22 ETU guard・APDU間保持・共有USB解放・無replay・S保留）:**
  - **位置付けと私有境界:**
    - Issue 25（コメント6886346683）で採用された案A（Core共通規則＋証拠付きFW保証または完全受信順序保証と安全な実効ETU上界を渡す私有契約）に基づき、S(RESYNCH)着手に先立って共通完成T=1 block交換口（`exchange_public_t1_block`）の送信前に22 ETUのBGT（Block Guard Time）保護を通す私有Core単位（C11中段）である。
    - 受信R-block再送（PR51）やCore送信Rによる系列回復（PR52）、および将来のS制御において、全I/R/S送信が同じ共通block送信入口を通り、APDU間でもBGT保護が維持される共通構造を確立する。なお、S(RESYNCH)制御自体の実装は保留中である。
    - 本BGT契約はCore内部の私有搬送契約であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言、正本、製品SPI（既存operation table/signature）は変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。また、実装の私有待機値や上限を新公開契約にしない。
    - 完成block（`complete_block`）におけるFWのframe/EDC検査保証と物理BGT保証は別概念であり、合成fixtureでの成功は実機ハードウェアにおける物理BGT保証の実証ではない。
  - **三状態capabilityとreader guard状態:**
    - **三状態capability:** `PublicCardTransportPlan` にreader私有の `CardBgtCapability` を導入し、以下の三状態を厳密に区別：
      1. `unknown`: 初期値。BGT保証の証拠なし。
      2. `transport_guarded`: 証拠ID（evidence ID）付きで物理搬送側がBGT guardを直接保証。
      3. `bounded_etu`: 完全受信順序保証（`receive_complete_after_last_edge = true`）および安全な実効ETU時間上界（`etu_duration_upper_bound_ns`）を提供。
    - **reader guard状態と境界失効:** Core内部のRootにconnection epoch、request epoch、完全受信完了時刻、guard期限、capability revision、ETU上界、mode/evidence IDを保持。resetや抜去（epoch更新）発生時は旧境界・旧revisionを失効。
  - **送信前guard待機とリソース所有・安全規則:**
    - **22×ETU算術検査とguard待機:**
      - ISO/IEC 7816-3:2006 §7.1（実効ETU）および§11.2（逆方向文字先頭間22 ETU）に基づき、完全受信完了時刻から `22 × 上界ETU` 経過まで送信を待機（BWT §11.4.3 とは別概念）。
      - 22倍算のオーバーフロー検査、および未知・0・overflow・revision/evidence不整合の事前検査を実施し、不正時は物理OUT前に拒否（送受信進行量0、結果既知を保持）。
      - byte/chunk内のfragment間には誤ってguardを挿入せず、block全体の送信前にのみ適用。
    - **待機中のリソース解放とcaller期限:**
      - 待機中はAPDUの既存 `CardT1SessionLease` と論理operation ticketを維持してcancel/close検知を可能としつつ、operation admissionを一時解放して共有USB endpoint資源を保持せず、別receiver 188 readやCARD通知の並行進行を阻害しない。
      - 次block送信時にcaller期限、cancel/close、epochを再検査して再admission。待機中にcaller期限に達した場合は物理OUTを出さずTIMEOUTとしてsessionを失効（APDU全体の自動再送・replayは行わない）。
    - **実送信operation planのBGT改定検査と未経過guard保護:**
      - 共通T=1 block送信前のBGT保護において、Core共通の内部before-send hookを追加し、actual operation plan（`complete_block` では実際の `CardT1BlockExchange` plan、`byte_chunks` では最初の `CardByteWrite` plan）のBGT capabilityを検査。
      - planのoperation outputおよびresource keysを待機前にCore所有vectorへコピーし、待機中に共有USB資源を保持せず、待機完了後にその同一actual planをそのままadmission・物理OUTへ使用（待機後の通常re-planningによるone-shot状態二重消費を回避）。
      - guard目標時刻に達した時点でplan callbackへ予約済み `CardByteRead{capacity=0}` を渡してBGT metadataをtiming-onlyで再照会（通常transport operationへは渡さない）。
      - 返却capabilityからrevision・上界・期限を再検査し、上界増加時は完全受信完了時刻を起点に待機期限を再計算・延長。Rootが保持する未経過期限とのmaxを維持し、縮小改定時も未経過期限を短縮しない。更新された上界・時刻値は完了記録（Root）にも反映。
      - 再照会不能、unknown、同revisionでのmode/evidence変更・上界不整合、およびrevision rollback発生時は物理OUT前に停止（fail-closed、送受信進行量0、結果既知を保持）。
      - ただし、製品adapterのplan callback副作用やcapacity-zero対応、未確認な全製品の対応、任意瞬間の物理clock変化の安全性までを全面保証するものではない。
  - **両adapterでの実測実績:**
    - 合成単調時計を用いたfixtureにおいて、完全受信後22,000,000nsに達するまで次APDU送信を待機すること、byte_chunksからcomplete_blockへのAPDU間guard保持、正しいINFと系列結果を確認。
    - 実送信plan改定の代表fixtureとして、初期1ms/rev10から旧期限22ms直前に5ms/rev12へ増加し、timing-only再照会を経てactual `CardT1BlockExchange` planが10ms/rev13を返した場合に、guardを受信完了起点220msへ延長し、220ms期限直前の1ms/rev14縮小でも短縮せず220msを維持してOUTし、正しいINFを返すことを確認（通常send plan改定反例を代表fixture化したもの）。
    - 上界増加後の新期限がcaller deadline（150ms）を超える場合のTIMEOUT（OUTなし、tx=0、結果既知）、unknown・上界0・22倍算overflow・同revision不整合・rollbackの拒否、重複I→R→正常Iの共通send入口通過、および待機中cancel時のCANCELLED回収を確認。
- **Core私有カード側T=1 response chainingと正常R ACK（PR55、C11中段・カードM=1連鎖受信・一時buffer蓄積・正常R ACK送出・最終M=0時INF確定・送受sequence更新規則・途中失敗部分応答なし/無replay/無S再同期・両adapter/3 block/CRC/BGT境界実測）:**
  - **位置付けと私有境界:**
    - PR50〜PR53のCore内部APDU交換・再送制御・BGT保護を基底とし、カード側からの長大応答（INF > IFSC）に対するT=1ブロック連鎖（`M-bit = 1`）をCore内部で一時bufferへ蓄積し、正常R ACK（`R(N(R), error=0)`）を返して `M-bit = 0` まで継続受信し、最終確定時にのみ全INFを連結してAPDU応答として返す私有Core単位（C11中段）である。
    - 本連鎖処理はCore内部の私有処理（`exchange_public_t1_apdu`）であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言、正本、製品SPI（既存operation table/signature）は変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。また、実装の私有buffer上限や連鎖長を新公開契約にしない。
    - 端末側からの長大コマンド送信（command chaining）、S-block制御/WTX/IFS/RESYNCH回復（保留中）、不完全/構造不正ブロックやBWTタイムアウトからのR生成、公開APDU送受信API、PC/SCは後続工程である。
  - **M=1応答受信と正常R ACKの送出:**
    - **M=1 I-blockの受信と一時蓄積:**
      - カードから妥当なI-block（NAD=0, 期待受信系列 `N(S)`, 有効EDC）を受信し、PCB bit 6（M-bit）が 1 の場合、そのINFをCore所有の一時バッファへ追加。
    - **正常R ACKの構築と共通BGT出口経由での送信:**
      - 次に期待するカード送信系列を示す `N(R)`（直前IのN(S)が0ならN(R)=1でPCB `0x90`、N(S)が1ならN(R)=0でPCB `0x80`、bits 4..1=0000: errorなし）を持つ正常R-block（NAD=00, LEN=00, INFなし, セッション選択EDC）を生成。
      - LRCセッションでは1バイトEDC、CRCセッションでは2バイトEDC（例: 最初の `I(0,1)` に対する `R(1)` は `00 90 00 91 df`）を付与。
      - 既存の同一APDU session lease、retained card ticket、caller deadline、1-worker経路を維持し、PR53で導入された共通block/BGT出口（`exchange_public_t1_block`）を経由して送信（22 ETU下限guardを適用）。APDU command Iの再送やAPDU全体の再入は行わない。
  - **最終応答確定と送受sequence更新規則:**
    - **M=0での全INF連結確定:**
      - カードから `M-bit = 0`（PCB bit 6 = 0）を持つ最終I-blockを受信・検証した時点で初めて、一時バッファに蓄積された全INFと最終I-blockのINFを結合して一つのAPDU応答として確定・返却。
    - **送受sequence更新規則と次APDUへの引き継ぎ:**
      - 送信sequence `N(S)` は、最初のcommand Iがカードにより確認された分（1回）のみトグル更新。
      - 受信期待sequence `N(S)` は、受理した妥当なI-blockごとに反転（トグル）更新。
      - 最終確定時に更新された送受sequenceをsessionへ保存し、次APDUへ引き継ぐ（例: 2 block連鎖では、初回のcommand I(0,0)に対してカードがI(0,1), M=1を返し、CoreがR(1)を返し、カードがI(1,0), M=0を返して完了した場合、受信期待系列は 0→1→0 と遷移し、次APDUはcommand I(1,0)から送信してカードI(0,0)を受理する）。
  - **途中失敗・重複応答の排除と結果既知性・無replay・無S再同期:**
    - **APDU失敗時の結果既知性（result_known）契約:** `exchange_public_t1_apdu` の共通 finish 処理において、APDU 累積 `tx_bytes_done` が非 0 かつ `reply_complete == false` の場合、APDU aggregate の `result_known` を `false`（結果不明）へ設定する。一方、送信前の 0 進行失敗（`tx_bytes_done == 0`）は、block 単体の契約どおり `result_known = true` を維持する（あらゆる 0 byte 送信前失敗まで結果不明とするものではない）。
    - **共通集約による全 APDU 失敗への一貫適用:** 本集約境界により、response chain 途中の cancel/close/timeout 等だけでなく、単一 I/R 交換の再送上限失効や重複 I/R 回復未完の失敗においても、command 送信後に完全な APDU 応答を受領できなかった場合は同様に結果不明（`result_known = false`）となる。
    - **単発重複 I の継続回復と上限失効:** 連鎖途中で重複 sequence を持つ I 応答を受信した場合、単発の重複であれば既存の error-indication R 回復を送信して正しい次の I 応答への回復を継続し、その重複 INF を一時バッファへ連結しない。重複応答が反復して既存の 3 回上限に達した時点で初めて、部分 INF を返さず `result_known = false`（tx 非 0・未完）として session を無効化（失効）する（単発重複 1 つで即座に失効するわけではない）。
    - **途中失敗と無 replay・無 S 再同期:** 系列不一致、予約 bit/不正 EDC、最終 I 欠損、R 部分送信、タイムアウト、cancel/close、epoch 不一致等が発生した場合、蓄積中の部分 INF を成功応答として返さず（`reply_complete = false`, `result_known = false`）、session を無効化（失効）。曖昧な APDU command I の自動再送（replay）は行わず、また自動 S 再同期も行わない（正本 §6.2 の無効化・新 reset による初期系列回復を維持）。
  - **両adapterでの実測実績:**
    - byte_chunks と complete_block の両adapterにおいて、2 block連鎖（`command I(0,0) -> card I(0,1), M=1 -> Core R(1), PCB=0x90/LEN=0/INFなし -> card I(1,0), M=0`）によるINF連結応答（INF-A||INF-B）、および次APDU（`command I(1,0) -> card I(0,0)`）への系列引き継ぎを確認。
    - complete_block において、3応答block連鎖（`I(0,1) -> R(1) -> I(1,1) -> R(0) -> I(0,0)`）を確認し、連鎖数が2固定でないことを確認。
    - LRCおよびCRCセッション双方で正常R ACK frame（CRC付きではPCB/LENおよびCRC 2バイト `91 df` を含むR(1)）を照合。
    - 合成単調時計により、応答受信後の最初のguarded submitが22 ETU（22ms）下限以後であること、3 block連鎖での各R ACK送信境界を確認。
    - 連鎖途中の BGT 待機中 cancel 実測: complete-block adapter において、command I 送信完了（tx 8 bytes）および M=1 card I 応答（rx 6 bytes）受信後、正常 R ACK の BGT 待機中に cancel された場合、`DTV_INTERNAL_RESULT_CANCELLED`、`tx=8`, `rx=6`, `result_known=false`, `reply_complete=false`、response 空（部分 INF なし）、endpoint OUT は command frame の 1 回のみ（追加の R ACK 送信なし）、command replay なしを確認。
    - 送信前 cancel 対照: 既存の送信前 BGT cancel fixture では `tx=0`, OUT なし, `result_known=true` が維持されることを対照確認。
    - 重複 I fixture: 単発重複 I が R 回復を経て正しい I へ回復すること、および反復重複 I が 3 回上限で `result_known=false`（tx 非 0・未完）となり部分 INF なし・command 1 度のみで session が失効することを確認。
- **Core私有T=1 command chainingと正常受信R ACK（PR56、C11中段・IFSC分割送信・途中M=1送信・正常R ACK受信・同N(R)現在片再送・最終M=0からresponse chain接続・送受sequence更新規則・途中失敗部分応答なし/無replay/無S再同期・両adapter 65byte/3片実測）:**
  - **位置付けと私有境界:**
    - PR50〜PR55を基底とし、端末からカードへの長大コマンド送信（INF > IFSC）において、Core内部でcommand全体長を拒否せず現在IFSC以下の連続片（slice）に分割し、途中片 `I(N(S), M=1)` に対するカードからの妥当な正常R ACKを受理して次片へ進め、最終片 `I(N(S), M=0)` をPR55の応答連鎖（response chaining）へ接続してAPDU交換を完了する私有Core単位（C11中段）である。
    - 本連鎖処理はCore内部の私有処理（`exchange_public_t1_apdu`）であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言、正本、製品SPI（既存operation table/signature）は変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。また、恣意的な公開command長上限を作らない。
    - S-block制御/WTX/IFS/RESYNCH回復（保留中）、不完全/構造不正ブロックやBWTタイムアウトからのR生成、公開APDU送受信API、PC/SCは後続工程である。
  - **IFSC分割送信と正常R ACKの受理規則:**
    - **IFSC単位の連続片分割とM=1設定:**
      - 入力command byte列を現在選択されているIFSC長（初期値32等）以下の連続片へ分割。
      - 最後の1片を除くすべての途中片に対して `M-bit = 1`（PCB bit 6 = 1）を設定したI-blockを構築し、セッション選択EDCを付与。
    - **妥当な正常R ACKの判定と次片進行:**
      - カードから受信したブロックが、NAD=0、PCB bits 8..7=`10`（R-block）、bit 6=`0`、bits 4..1=`0000`（エラーなし）、LEN=0、INFなし、予約bit=0であり、かつ受信 `N(R)` が直前に送信したI-blockの次の送信系列 `(N(S) + 1) % 2` と一致する場合に限り、妥当な正常R ACKとして受理。
      - 送信offsetを進め、送信sequence `N(S)` を反転（トグル）更新して次片の送信へ移行。
    - **同一N(R)による直前同一片の再送要求と上限:**
      - 受信した妥当なR-blockの `N(R)` が直前送信の `N(S)` と同じ場合（再送要求）、直前の同一符号化I-block（pre-encoded frame）だけを再送。
      - ISO/IEC 7816-3 §11.6.3.2 に従い最大3試行内で再送を実施。過去にACK確認済みの片やAPDU全体の曖昧な再送（replay）は一切行わない。
  - **最終片M=0からresponse chainingへの接続とsequence確定:**
    - 最後の1片に対しては `M-bit = 0`（PCB bit 6 = 0）を設定したI-blockを送信。
    - カードからの応答はPR55で確立された応答連鎖処理へそのまま接続され、カード側がM=1なら正常R ACKを返してM=0まで継続受信し、最終確定時にのみ全INFを連結。
    - APDU成功確定時に、進められた送受双方のsequenceをsessionへ保存し、次APDUへ引き継ぐ。
  - **途中失敗と結果既知性・無replay・無S再同期:**
    - 途中片送信時における予約bit不正R、早期I応答、R ACK欠損（TIMEOUT）、部分OUT、cancel/close、epoch不一致等の発生時は、部分INFを成功応答として返さず（`reply_complete = false`）、累積 `tx_bytes_done != 0` に応じて `result_known = false`（結果不明）とし、sessionを無効化（失効）。
    - 曖昧なcommand replayや自動S再同期は行わず（正本§6.2の無効化・新resetによる初期系列回復を維持）、同一期限、retained ticket、session/epoch、共通BGT入口（`exchange_public_t1_block`）、1-worker経路を維持。
  - **両adapterでの実測実績:**
    - byte_chunks と complete_block の両adapterにおいて、IFSC 32・command長65 bytesの3片連鎖（`32 / 32 / 1` bytes、`I(0,1) -> R(1) -> I(1,1) -> R(0) -> I(0,0) -> card I応答 (11 22 90 00)`）および次APDU（`I(1,0)`）への系列引き継ぎを実測確認（LRCセッション）。
    - 正常R ACK（N(R)進捗）と同一N(R)再送要求の対照を確認（再送時は現在片のみ再送され、ACK確認済み片やAPDU全体の再送なし）。
    - 異常系実測: 予約bit不正R・早期I応答（USB_IO）、R ACK欠損（TIMEOUT、tx=36, rx=0, known=false）、部分OUT（tx=8, rx=0, known=false）、BGT待機中cancel（CANCELLED、tx=36, rx=4, known=false）を確認。
    - BGT動作確認: 各command I frameの送信（submit）時刻を採取し、第2片および第3片の送信が直前送信から少なくとも22 ETU（合成1ms ETU上界にて22ms）経過していることを確認（注: これは各command OUT間の比較検証であり、独立したR ACK IN完了から次I OUTまでの直接測定やハードウェア物理保証ではない）。
    - なお、CRCセッションでの新command chain専用動的試験は未実施（LRCセッションにて実測）。
- **Core私有カード起点S(IFS)と端末S(IFS response)（PR57、C11中段・カード起点S(IFS request)受理・端末同値S(IFS response)送出・カード後続IでAPDU完了・新IFSC保持と次APDU分割反映・S-blockでI系列不変・途中失敗部分応答なし/無replay/無S再同期・両adapter LRC/CRC代表/22 ETU BGT実OUT境界実測）:**
  - **位置付けと私有境界:**
    - PR50〜PR56（単一I交換、R再送、系列回復、BGT保護、応答連鎖、コマンド連鎖）を基底とし、カード側起点によるS(IFS request)（PCB `0xC1`, LEN `1`, INF `0x01..0xFE`）を受領した際、Core内部で同じINF値を持つS(IFS response)（PCB `0xE1`, LEN `1`, 同一INF, セッション選択EDC）を生成し、共通BGT出口（`exchange_public_t1_block`）経由で返送し、続くカードからの後続I応答を受領して現在APDUを正常完了させる私有Core単位（C11中段）である。
    - APDU成功確定時に、確定した新IFSCをセッションへ保持し、次APDUの実command chainingにおいて新しい片長制限（新IFSC）として反映する。
    - S-block交換自体はI-block送受系列番号（send_sequence, receive_sequence）をトグル更新せず、APDUのINFバイトとしても計上しない。
    - 本S(IFS)処理はCore内部の私有処理（`exchange_public_t1_apdu` / `EnclosureRegistry`）であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言、正本、製品SPI（既存operation table/signature）は変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。また、実装の私有待機値や上限を新公開契約にしない。
    - 端末（host）起点によるIFS要求/IFSD提案、S(WTX)およびATR由来BWT/CWT待機、S(ABORT/RESYNCH)等の規格内回復、途中自動S(RESYNCH)（ユーザーA判断 Issue 25: 6890319472 / 正本 §6.2 により保留維持）、公開APDU送受信API、PC/SCは後続工程である。
  - **S(IFS)往復と新IFSC保持・次APDU分割反映:**
    - **S(IFS request)の受理規則:**
      - 最終command I送信後、カード応答位置において、NAD=0, PCB=`0xC1`（S(IFS request)）, LEN=`1`, INF=`0x01..0xFE`（1〜254バイト、0x00および0xFFは予約・不正）, セッション選択EDC妥当なブロックを受理。
      - 途中片での中間S要求、予約PCB、方向不正（Core受信時のE1）、LEN != 1、不正IFS（00/FF）、EDC不正は受理しない。
    - **同値S(IFS response)の構築と共通BGT出口経由での送信:**
      - 受領したIFS要求と同一のINFバイト（1バイト）、PCB=`0xE1`（S(IFS response)）、NAD=0、LEN=1、セッション選択EDC（LRCまたはCRC）を持つS応答ブロックを生成。
      - 同一session lease、retained card ticket、caller期限、epoch、1-worker経路を維持し、PR53の共通block/BGT出口（`exchange_public_t1_block`）を経由して送信（22 ETU下限guardを適用）。
    - **カード後続I応答によるAPDU完了と新IFSC確定:**
      - S応答送出後、カードから届く後続I-block応答を受理・検証し、現在APDUを完了。
      - APDU成功確定時に、送受I系列とともに確定した新IFSCをsession stateへ保存・反映。
      - S-block交換ではI系列を進めない（送受sequence不変）。
      - reset、抜去、epoch更新、APDU失敗時はIFSCをクリア（初期値または破棄）。
    - **次APDUでの新IFSC分割反映:**
      - 次APDUにおいて、新IFSC（例: 初期値32から16へ変更）に従い、長大コマンド（例: 33バイト）が新片長（16/16/1の3片）へ分割送信され、正常R ACK（R(0), R(1)）を受理して全送出が完了することを確認。
  - **異常系・途中失敗と結果既知性・無replay・無S再同期:**
    - **異常要求の拒否:** 不正IFS（00, FF）、方向違いE1、LEN/INF形状不正、予約PCB、EDC不正に対してはS応答を返さず、部分応答なし、session失効。
    - **S応答OUT部分送信・BGT待機中cancel・後続I欠損:**
      - S応答のpartial OUT時はUSB_IO（tx=10, rx=5, response empty, result_known=false, reply_complete=false）。
      - S応答のBGT待機中cancel時はCANCELLED（tx=8, rx=5, response empty, result_known=false, reply_complete=false、S応答OUTなし）。
      - S応答送信後、後続I応答欠損時はTIMEOUT（tx=13, rx=5, response empty, result_known=false, reply_complete=false）。
      - いずれも累積tx非0かつ未完のため `result_known = false`（結果不明）、部分INF返却なし、曖昧なcommand replayなし、自動S再同期なし（正本§6.2維持）。
  - **両adapterでの実測実績:**
    - byte_chunks と complete_block の両adapterにおいて、LRCセッションでの正常S(IFS)往復（`command I(0,0) -> card S(IFS request, INF=16) -> Core S(IFS response, INF=16) -> card I(0,0) 応答`）および次APDUでの33-byte commandの16/16/1分割送信（`I(1,1,16) -> R(0) -> I(0,1,16) -> R(1) -> I(1,0,1) -> card I応答`）の実測成功を確認（tx=45, rx=16）。
    - complete_block において、CRCセッションでの正常S(IFS)往復および同値E1 INFのCRC送出（tx=15, rx=15）を確認。
    - BGT測定境界: 両adapterにおいて、合成単調時計を用いてカードS(IFS request)のIN完了時刻からCore S(IFS response)の実際のOUT提出（submission）時刻までの差を測定し、設定された1,000,000 ns/ETUに対し22 ETU（>=22,000,000 ns）経過していることを実測確認（単なる計画準備時刻や前OUT→次OUTの比較ではなく、IN完了から実OUT提出までの境界を確認）。
    - 異常系実測: 不正IFS（00, FF）、方向不正E1、LEN不正、予約PCB、EDC不正（LRCエラー時tx=8, rx=5, E1送出なし）、partial OUT（tx=10, rx=5）、BGT待機中cancel（tx=8, rx=5, E1送出なし）、後続I欠損timeout（tx=13, rx=5）において、response empty、reply_complete=false、result_known=false、追加replayなし、session失効を確認。
- **Core私有host起点S(IFS)によるIFSD縮小交渉と応答連鎖（PR58、C11中段・根拠付き完成block容量/EDC算術・1..31縮小時のみ初APDU前C1/E1交渉・新IFSD確定と16+4応答連鎖・SでI系列/IFSC不変・Sのみ失敗時known/command送信後unknown・complete-block EDC検査共通有効化・両adapter LRC/CRC代表/22 ETU実OUT境界実測）:**
  - **位置付けと私有境界:**
    - PR50〜PR57を基底とし、受信側ハードウェアまたは物理搬送の制約等により安全に受信可能な完成T=1ブロック容量が初期IFSD（32バイト）より小さいセッションにおいて、Core内部で最初のAPDUのcommand Iを送信する前にhost起点S(IFS request)（PCB `0xC1`, LEN `1`, INF `1..31`）を送出し、カードからの同値S(IFS response)（PCB `0xE1`, LEN `1`, 同一INF, セッション選択EDC）を受理してIFSDを縮小確定し、その縮小上限に従うカード応答連鎖（16+4の20バイト応答）を受理してAPDUを完了する私有Core単位（C11中段）である。
    - 本交渉処理はCore内部の私有処理（`exchange_public_t1_apdu` / `EnclosureRegistry`）であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言、正本、製品SPI（既存operation table/signature）は変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。また、fixtureで観測した16は状態遷移を実証するための合法値であり、実製品の固定既定値や実機容量保証ではない。
    - 受信容量32超へのhost IFS拡大、ATR由来BWT/CWTと物理文字edge・不完全block分類、S(WTX)の倍率と新待ち起点、その他S(ABORT/RESYNCH)等の規格内回復、途中自動S(RESYNCH)（ユーザーA判断 Issue 25: 6890319472 / 正本 §6.2 により保留維持）、公開APDU送受信API、PC/SCは後続工程である。
  - **根拠付き容量申告とCore算術・交渉発火条件:**
    - **根拠付き完成block byte容量の入力:**
      - 製品plan（`PublicCardTransportPlan`）に互換的な私有能力 `CardT1ReceiveCapacity`（`complete_block_bytes`, `revision`, `evidence_id`, `known`）を追加。
      - 申告される単位は「安全に受信可能な完成T=1ブロック全体のバイト数」であり、USB transfer buffer長とは扱わない。
      - 未知値は全数値・識別子ゼロで表し、既知値には正当なrevisionおよびevidence IDが必須。値や根拠が不明な場合は安全保証を主張せず、S交渉も発火させない。
    - **CoreによるINF上限算術:**
      - Coreは完成blockバイト数からprologue 3バイト（NAD/PCB/LEN）およびsession選択EDC（LRC: 1バイト、CRC: 2バイト）を減算し、安全に受信可能なINFバイト上限 `candidate_ifsd = min(254, complete_block_bytes - 3 - edc_bytes)` を算出。
      - 算出した上限が32以上または能力不明（unknown）の場合は余分なS送信を行わず、初期IFSD=32のまま既存のcommand I送出経路を維持。
      - 算出した上限が1〜31の範囲にある場合のみ、最初のAPDUのcommand I送出前にhost起点S(IFS request)を発火。
  - **S(IFS)往復と新IFSD確定・応答連鎖への反映:**
    - **S(IFS request)の送出と同値S(IFS response)の受理:**
      - PCB=`0xC1`、LEN=`1`、INF=`candidate_ifsd`、NAD=`0`、セッション選択EDCを持つS(IFS request)ブロックを構築し、共通block/BGT出口（`exchange_public_t1_block`）経由で送出。
      - カードからNAD=0, PCB=`0xE1`, LEN=1, INFが提示値と完全一致, セッションEDC妥当なS(IFS response)を受信した場合に限り、`update_card_t1_ifsd` により新IFSDを確定してprivate sessionへ保持。
      - S-block往復はI系列番号（send_sequence, receive_sequence）を進めず、IFSCも変更しない。
      - 同一session lease、card ticket、caller絶対期限、epoch、BGT経路を維持。reset、抜去、epoch更新、APDU失敗時は交渉済みIFSDを破棄（初期値32へ戻る）。
    - **新IFSDに基づくカード応答連鎖の完了と次APDUへの引き継ぎ:**
      - 成功したS交換の後、同じcaller内で最初のcommand I（`I(0,0)`）を送信。
      - カードからの応答は新IFSD（16）に従い、16バイト片（`I(0,1), M=1`）および4バイト片（`I(1,0), M=0`）の20バイト連鎖応答として返され、Coreの正常R ACK（`R(1)`）を経てAPDU応答として完了。
      - 次APDUではcommand I（`I(1,0)`）から開始され、新IFSDが維持されて余分なS交渉は再送されない。
  - **command送信前後の結果既知性（result_known）とcomplete-block EDC検査:**
    - **command未送信時のAPDU結果既知性（known=true）:**
      - 最初のcommand I送出前にS(IFS request)だけを送って障害（S response値違い/方向違い/不正LEN/EDC、タイムアウト、partial OUT、cancel等）が発生した場合、物理tx総量（tx_bytes_done）が非0であっても、APDUコマンド本体はカードへ未到達であることが確定しているため、APDU aggregateの `result_known = true`（結果既知）を維持し、空responseを返し、sessionを無効化（失効）。
    - **command送信後失敗時の結果不明性（known=false）:**
      - command Iを送出した後で障害（IFSD超過応答、R欠損、タイムアウト等）が発生した場合は、累積tx非0に応じて既存の契約どおり `result_known = false`（結果不明）、空response、session失効。全APDU自動replayや途中自動S(RESYNCH)は行わない（正本§6.2維持）。
    - **complete-block EDC検査の共通有効化:**
      - complete-block受信経路においてEDC検査が未実施（破損E1を受理してしまう不具合）であった実装漏れを特定・修正し、Core共通のEDC検査をcomplete-blockでも有効化して破損応答を確実に拒否するよう修正。代表回帰を確認（先行PRや全製品で既に全EDCを実測済みだったとは読み替えない）。
  - **両adapterでの実測実績:**
    - byte_chunks と complete_block の両adapterにおいて、LRCセッションでの正常S(C1, INF=10)→カードS(E1, INF=10)→command I(0,0)→カードI(0,1, INF16)→Core R(1)→カードI(1,0, INF4)による20バイトAPDU応答および次APDU I(1,0)への引き継ぎを実測確認（tx=17, rx=33 bytes）。
    - complete_block において、CRCセッションでの正常往復を確認（tx=20, rx=36 bytes。E1 6 + I(0,1) 21 + I(1,0) 9 = 36 bytes）。
    - BGT測定境界: 両adapterにおいて、合成単調時計を用いてS response IN完了時刻からcommand I実OUT提出時刻、およびchained I IN完了時刻からR実OUT提出時刻までの差を測定し、それぞれ設定された22 ETU（>=22,000,000 ns）経過していることを実測確認（単なる計画時刻や前OUT→次OUTの比較ではない）。
    - 異常系実測: 不正容量（0、overflow）、IFS 00/FF、S response値違い/方向違い/不正LEN/EDC/NAD、partial OUT、missing response、cancel、IFSD超過応答において、command未送信時のresult_known=true対照およびcommand送信後のresult_known=false失効を確認。
- **Core私有根拠付きBWT判定とI後タイムアウトのR回復（PR59、C11中段・根拠付き文字edge/不確定境界/規格R回復/合成測定の帰属・ATR BWI既定4と有効F/D算術・真正absence timeout時のI→R→I・期待カード受信系列NRとR error field・有界R再試行・BWT境界直前edgeとcallback遅延・両adapter LRC/CRC代表・系列分離回帰・直前IN完了基準22ms実測と物理edge未測定帰属）:**
  - **位置付けと私有境界:**
    - PR50〜PR58を基底とし、ATR由来のBWT（Block Waiting Time）と根拠付き物理文字時刻（leading edge）を用いてI-block送信後の最初のカード文字を判定し、期限内の正常I→IでAPDUを完了し、確定BWT超過時にはISO/IEC 7816-3規格に従い元I-blockを再送せず、期待するカードI-block受信系列を持つR-block（`R(N(R)=expected_card_receive_sequence)`、実フレーム low nibble は `0010`: other errors）を送出して期待I応答を求め同一APDUを完了する私有Core単位（C11中段）である。
    - 本判定・回復処理はCore内部の私有処理（`exchange_public_t1_apdu` / `EnclosureRegistry`）であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言、正本、製品SPI（既存operation table/signature）は変更しておらず、公開APDU完成やT=1規格全体準拠と取り違えない。また、fixtureで観測した時間は合成値であり、実製品ハードウェアでの物理文字edge保証や実BWT保証ではない。
    - S(WTX)の倍率と新待ち起点、CWT（同一ブロック内の連続文字間edgeと不完全block分類）、受信容量32超へのhost IFS拡大、その他S(ABORT/RESYNCH)等の規格内回復、途中自動S(RESYNCH)（ユーザーA判断 Issue 25: 6890319472 / 正本 §6.2 により保留維持）、公開APDU送受信API、PC/SCは後続工程である。
  - **ATR BWIとBWT算術・妥当性検査:**
    - **ATRパラメータの抽出とBWI既定値:**
      - ATRのT=1用TBからBWI（Block Width Index: bits 8..5、欠如時は規格既定値4）を抽出し、TA1から有効なF, Dを保持。
      - ISO/IEC 7816-3 §11.4.3 に基づき、Core共通処理として `BWT = ceil((11*F + D * 2^BWI * 960 * 372) * 1e9 / (D * card_clock_hz))`（ns）を算出。
      - BWI予約値（BWI > 9）、無効なF/D（未定義・0）、無効なcard clock（0）、算術overflowを妥当性検査し、不正時はT=1選択を拒否（根拠のない固定ミリ秒値やcaller deadlineをBWTの代用にしない）。
  - **根拠付き物理文字edge入力と判定分類:**
    - **私有時間証拠の入力:**
      - 物理搬送側（私有plan）から同一clock domainにおけるhost block最終文字leading edge、次card block最初文字leading edge（または確定absence観測時刻）、および測定誤差上界（error bound）を、evidence ID・plan revision・reader epoch・transactionと結び付けて申告。
      - BGT用のIN完了時刻やETU上界、USB callback時刻を物理文字edgeそのものへ読み替えない。
    - **CoreによるBWT境界判定分類:**
      - 境界誤差を合算し、上限がBWT以内（`elapsed_ns + error_sum_ns <= bwt_ns`）ならtimely（時間内）。
      - 下限が超過（`elapsed_ns - error_sum_ns > bwt_ns`）なら確定timeout。
      - 境界を跨ぐ場合（`elapsed_ns - error_sum_ns <= bwt_ns && elapsed_ns + error_sum_ns > bwt_ns`）はuncertain（不確定）。不確定境界を時間内受信や確定timeoutと偽らない。
      - clock、clock domain、error_bound_known、revision、evidence_id、connection_epoch、request_epoch、transaction_idが不足・不一致の場合は保証なし（unavailable）として扱う。
  - **確定BWT timeout後のR回復と期待受信系列NR・R error field・有界再試行・無replay:**
    - **規格R回復（I→R→I）と期待受信系列NR・R error field:**
      - ISO/IEC 7816-3 §§11.3.2.2, 11.6.2.3 rule 3, 11.6.3.1, 11.6.3.2 rule 7.1 に従い、R-block の $N(R)$（bit 5）は期待する相手側（カード）I-block の系列番号を示し、host 送信 I 系列とは独立する。
      - 最初のカード文字が BWT 内に無いと確認でき（確定 absence）、かつ caller/epoch が有効なら、元 I-block を自動再送（replay）せず、期待するカード受信系列 $N(R)$（`receive_sequence`）を持つ R-block を構築し、共通 block/BGT 出口（`exchange_public_t1_block`）経由で送出してカードからの I 応答を待つ。
      - **NR と R error field の区別および規格分類:**
        - R-block PCB（bits 8..1）において、bit 5 の $N(R)$ と bits 4..1 の error field は別フィールドとして定義される。
        - ISO/IEC 7816-3 §11.3.2.2 の規格分類では、bits 4..1=`0000` は "Error-free acknowledgement"、`0001` は "EDC and/or parity error"、`0010` は "Other errors" である。
        - Core の実 frame PCB は `0x82`（NR=0 時）または `0x92`（NR=1 時）であり、low nibble（bits 4..1）は `0010`（other errors）である。
        - 以前の文書にあった「bits 4..1=0000: エラーなし再送要求」という記述を、実 frame および規格分類に合わせて訂正（実 frame は `0010`、`0000` ではない）。
        - 規格は BWT 超過を receiver-detected error に分類し rule 7.1 で期待 I を要求する R 送信を規定するが、timeout を `0010` へ対応づける規格文言の強さは留保されており、実装のプロトコル不適合が確定したとは断定しない。
      - 送信 I 系列と期待受信 I 系列が異なる有効履歴（2-I card response chain 後に host 次送信 1 / カード期待受信 0 の状態で次 APDU の BWT absence が発生した反例）において、誤った $R(N(R)=1)$ ではなく規格に従う $R(N(R)=0)$（実 frame `[00 82 00][82]`）を送出することで、要求された I(0) 応答を受け同一 APDU を完了し、次 APDU へ系列を引き継ぐ。
    - **R経路のBGT境界と合成観測実績:**
      - R 送信は既存 `exchange_public_t1_block` の送信前/BGT ゲートを通り同一 ticket・期限・epoch が維持される。
      - 独立 QA の合成 fixture timing probe において、両 adapter で直前 IN 完了から R OUT 提出まで 22,000,000ns（22ms）経過していることを実測確認（byte_chunks: prior IN 完了から 22ms 後の absence callback と同時刻に R OUT 提出、complete_block: prior IN 完了から 22ms 後に R OUT 提出。absence 観測後にさらに 22ms を足して待つのではない）。
      - ただし、これは合成 fixture 時計および USB submission 観測であり、実機ハードウェアでの物理 edge 保証ではない。
    - **有界再試行とsession失効:**
      - R送信後も再度BWT timeoutが発生した場合は、ISO/IEC 7816-3 §11.6.3.2 に従い最大3回のR送信で打ち切り、sessionを無効化（失効）。
      - 回復成功時はAPDU `result_known = true` でAPDUを正常完了。
      - APDU全体の曖昧な再送や途中自動S(RESYNCH)は行わない（正本§6.2維持）。物理欠落・cancel・caller期限切れはBWT timeoutと混同せず既存result_known/公開error規則へ渡す。
  - **両adapterでの実測実績:**
    - byte_chunks と complete_block の両adapterにおいて、根拠付きBWI（4）・有効clock（4MHz）・同一時刻系を与え、I(0,0, command)の実block OUT後、BWT内にカードI(0,0, response)の最初文字を観測して完全blockを受領し、APDU byteと次APDUの系列を確認（LRCセッション）。
    - 物理文字edgeはBWT境界直前（1ms）に届き、完成block callbackが20ms後に遅延到着する例においても、物理edgeに基づき正常にtimelyと判定されることを実測確認。
    - 確定absenceによるI→BWT超過→R→I回復、R後の再timeoutと有界3回再R、次APDU遷移を確認。
    - 送信I系列と期待受信I系列が異なる反例回帰（2-I response chain後のhost送信1/期待受信0でのBWT absence）において、元I再送なし、$R(N(R)=0)$ 送出、期待I(0)受領によるAPDU完了・次APDU遷移を確認。
    - clock不明（0）、clock domain不明、誤差上界不明、誤差幅がBWT境界を跨ぐ場合の拒否/uncertainを確認。
    - 回復後のreply_complete/result_knownおよび元Iの再送なしを確認。
- **Core私有カード起点S(WTX)確認と倍率付きBWT後のI応答（PR60、C11中段: S(WTX)検証・同倍率E3実OUT・E3最終host文字edge起点倍率BWT・通常BWT後延長内I受領・真正超過時規格R回復・caller絶対期限維持・両adapter LRC/CRC代表・合成edgeと物理未測定の帰属）:**
  - **位置付けと私有境界:**
    - PR50〜PR59（BWT算術と根拠付き文字edge判定、I後timeoutの期待R回復）を基底とし、先行I-block送信後の現BWT内にカードから届くS(WTX request: PCB `0xC3` / LEN 1 / 1-byte INF=[倍率])をCore私有で妥当性検査し、同一倍率のS(WTX response: PCB `0xE3` / LEN 1 / 同値INF=[倍率] / session EDC)を実blockとして送信。
    - S(WTX response)の最終host文字leading edgeを起点として `BWT * wtx_multiplier` の延長BWTを算出し、通常BWTを超過するが延長時間内に届くカードI-blockの最初文字をtimelyと判定して受領し、元のAPDUおよび次APDUを完了する私有Core単位（C11中段）である。
    - 本処理はCore内部の私有処理（`exchange_public_t1_apdu` / `exchange_public_t1_block`）であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言、正本、製品SPI表/署名は変更しておらず、既存の8引数 `exchange_public_t1_block` 呼び出しも forwarding overload により完全に維持。また、fixtureで観測した時間は合成値であり、実製品ハードウェアでの物理文字edge保証や実BWT/BGT保証ではない。
    - CWT（同一ブロック内の連続文字間edgeと不完全block分類）、受信容量32超へのhost IFS拡大、その他S(ABORT/RESYNCH)等の規格内回復、途中自動S(RESYNCH)（ユーザーA判断 Issue 25: 6890319472 / 正本 §6.2 により保留維持）、公開APDU送受信API、PC/SCは後続工程である。
  - **WTX倍率妥当性検査・S(WTX response)送出・算術:**
    - カードから受信したS(WTX request)について、NAD=0、PCB=`0xC3`、LEN=1、INF長=1、raw_block_length妥当性、EDC妥当性を検査。
    - ISO/IEC 7816-3 §11.4.3 に従い、倍率INF[0]は正の1-byte全域（1..255）を受理し、0のみを拒否（session無効化、空response、`result_known = false`, `reply_complete = false`, `USB_IO` で失効。倍率0を有効な延長として扱わない）。旧実装・旧文書の「1..59」「60以上未定義・予約拒否」は、ISO本文に59の上限規定がなく採用済み仕様にも根拠がなかったため、ユーザー判断（Issue 25: 6901870056）により推奨A（1..255受理・0のみ拒否、caller期限維持、積overflow失敗、正本不変）として改定・統一（59上限をISO規定や旧ユーザー決定とみなさない）。
    - BWT（ns）と倍率（1..255）の乗算において、64bit整数オーバーフロー検査（`*duration_ns > MAX / multiplier`）を実施。倍率が1..255の有効範囲であっても、時間の積（BWT * 倍率）がuint64（ns）の表現範囲（`UINT64_MAX`）を超える場合は失敗（UNSUPPORTED）として扱い、E3送出なし・session無効化・空response・`result_known = false`, `reply_complete = false` で失効。1-byteの倍率上限（255）と64bitの時間積上限（UINT64_MAX ns）を混同しない。
    - 同値倍率INF（1..255）を持つS(WTX response: PCB `0xE3` / LEN 1 / INF=[倍率] / EDC)を構築し、既存block送信口（`exchange_public_t1_block`）経由で送出。カードS受信完了からhost S実OUT提出には既存BGTを適用。
    - S往復（C3/E3）では host/card 双方向のI送受信系列番号（$N(S)$ / $N(R)$）を進めず、IFSCおよびIFSDも不変。
    - 同一の caller 絶対期限（timeout）・ticket・epochを維持し、WTXによって caller 絶対期限そのものを延長しない。
  - **延長BWT判定・真正timeout規格R回復・無replay:**
    - 物理搬送側（私有plan）から、S(WTX response)の最終host文字leading edgeを同一 clock domain・誤差上界・evidence ID・plan revision・reader epoch・transactionに結び付けて申告。USB callback時刻やOUT completion時刻を物理文字edgeそのものへ読み替えない。
    - Coreはその最終host文字edgeから `BWT * wtx_multiplier` の延長期限を計算し、次カードblock最初文字leading edge（または確定absence）をPR59と同一の根拠付き境界判定へ通す。
    - 連続するWTX request（反復WTX）も同様の規則でループ処理し、任意のWTX回数上限を勝手に設けない（caller絶対期限と既存失効境界で有界停止）。
    - 最初のカード文字が延長BWT内であれば、完成blockを既存のI応答・連鎖/APDU判定へ渡し、正常にAPDUを完了。
    - 延長BWTを確定超過した真正absence timeout時は、元I-blockを自動再送（replay）せず、ISO/IEC 7816-3 §11.6.3.2 rule 7.3 に従い期待カードI系列番号 $N(R)$（`receive_sequence`）を持つ R-block（実フレーム low nibble `0010`: other errors）を実OUTしてカードからのI応答を要求し、有界再試行回復へ接続。
    - 待機中もUSB admissionを常時排他保持せず、別receiverのTS受信を進める。
  - **両adapterでの実測実績:**
    - byte_chunks と complete_block の両adapterにおいて、LRC代表およびCRC代表での正常I→S(C3, 倍率2, 60, 255)→S(E3, 同倍率)→延長内カードI(0,0)受領・同一APDU完了・次APDU成功を実block送受で確認。
    - 算術オーバーフロー試験: 有効ATR（BWI=9, F=2048, D=1, clock 1Hz、BWT 182,867,968,000,000,000 ns）、倍率101（積 18,469,664,768,000,000,000 ns > UINT64_MAX 18,446,744,073,709,551,615 ns）において、C3受領後にUNSUPPORTED失効（tx=9, rx=6, response空, result_known=false, reply_complete=false, E3なし）を確認。
    - 反復WTX（C3/E3の連続往復後に同一I受領・APDU成功）、S(C3)受信後の完成callback遅延（1500ms、caller期限5000ms）、延長BWT境界直前edge（延長上限2859006000nsに対し経過2859005000ns、誤差2ns）でのtimely受領を確認。
    - 延長BWT確定超過時の規格R回復（元I再送なし、$R(N(R)=0)$ 送信、期待I応答受領）、倍率0拒否、部分E3 OUT、E3送出前cancel、request epoch不一致、caller期限切れTIMEOUT（sent_e3=false）での失効・knownnessを確認。
- **Core私有完成応答blockのCWT判定と通常I応答（PR61、C11中段: CWI/CWT算術・完成block全octet隣接leading-edge gap上下界照合・timely判定・CWT超過/不確定境界失効・余剰octet判定順保持・両adapter LRC/CRC代表・合成edgeと物理未測定の帰属）:**
  - **位置付けと私有境界:**
    - PR50〜PR60（BWT算術と根拠付き文字edge判定、I後timeoutの期待R回復、WTX倍率全域1..255とoverflowガード）を基底とし、先行I-block送信後にBWT内で始まったカードの完成応答block（NAD, PCB, LEN, INF, EDC）について、隣接する物理文字leading edge間隔（gap）をATR由来CWTで判定するCore私有処理（C11中段）である。
    - 本処理はCore内部の私有処理（`exchange_public_t1_apdu` / `scan_card_t1_reply`）であり、公開C API（`dtv_transmit_apdu`）の接続完了ではない。公開C宣言、正本、製品SPI表/署名は変更しておらず、既存SPI操作表・callback署名との互換性を完全に維持。また、fixtureで観測した時間は合成値であり、実製品ハードウェアでの物理文字edge保証や実CWT/BWT/BGT保証ではない。
    - 不完全blockのCWT lapse、block終端判定、無通信・送信権の取得と安全な規格R回復、受信容量32超へのhost IFS拡大、その他S(ABORT/RESYNCH)等の規格内回復、途中自動S(RESYNCH)（ユーザーA判断 Issue 25: 6890319472 / 正本 §6.2 により保留維持）、公開APDU送受信API、PC/SCは後続工程である。
  - **CWI/CWT算術・証拠照合・判定規則:**
    - ATRのT=1用TB3低nibble（bits 4..1）からCWI（Character Waiting Time Integer: 0..15、欠如時既定13）を取得し、TA1のF/Dおよびカードクロック周波数から、ISO/IEC 7816-3 §11.4.3準拠のCWT（ns）を算出:
      $$CWT = \left\lceil \frac{(11 + 2^{CWI}) \cdot F \cdot 10^9}{D \cdot \text{clock}} \right\rceil \text{ ns}$$
      算術例:
      - F=372, D=1, clock=4MHz のとき:
        - 既定 CWI=13 で $CWT=762,879,000 \text{ ns}$（約 762.88 ms）
        - CWI=0 で $CWT=1,116,000 \text{ ns}$（約 1.12 ms）
      - F=1024, D=1, clock=4MHz のとき:
        - 既定 CWI=13 で $CWT=2,099,968,000 \text{ ns}$（約 2.100 秒）
        - CWI=14 で $CWT=4,197,120,000 \text{ ns}$（約 4.197 秒）
        - CWI=15 で $CWT=8,391,424,000 \text{ ns}$（約 8.391 秒）
      （旧 F=372 と F=1024 の算術を混同しない）。
    - CWI/BWIや先頭文字BWTと内部CWTを混同しない。BWTは先頭カード文字、CWTは同じblock内の2文字目から最後のEDC文字までの隣接間隔を担当する。ISO/IEC 7816-3 §11.4.3 において bits 4..1 全域（0..15）が CWI としてエンコードされ、BWI と異なり 14 や 15 の予約規定は規格に存在しない。旧実装が無根拠に導入していた `cwi > 13` 予約拒否は独立 QA（成果 `a6cb4c6f`）で不合格となり、作者コミット `fecc4ec2` により 0..15 全域を受理するよう修正された経緯を記録。
    - 既存私有SPIの `CardT1BwtEvidence` を拡張し、完成応答blockの全octet数 $N$ に対する $N-1$ 個の隣接物理文字leading-edge gap上下界を、同一clock domain・誤差上界・evidence ID・plan revision・connection/request epoch・host/card transaction IDと結び付けて照合。
    - 誤差上界を保守的に広げ、上界 $\le CWT$ は timely（受理）、下界 $> CWT$ は exceeded（確定超過）、境界を跨ぐ区間は uncertain（不確定）、証拠欠落やdomain/count不一致は unavailable（保証なし）。
    - timely 以外は成功と判定せず、空response・既存knownness（未送信時true、送信後false）・session失効へ。元I-blockの自動再送（replay）なし。
    - CWT超過や未判定時に、追加の物理無通信証拠なしでRや途中S(RESYNCH)を新送信せず、元Iのreplayなし（不完全block終端・無通信/送信権・安全R回復は後続、正本§6.2/途中自動S保留維持）。
    - 判定順と既存エラー分類の保持: byte_chunks では LEN/EDC で完成blockを確定し、余剰octet検査を行った後に CWT を判定することで、既存の余剰データ `USB_IO` 分類を維持。CWT evidence不整合は成功させない。scopeの意味を判定順だけの仕様として断定しない。
    - 待機中も同一ticket/epochを維持しながら別receiverを進め、caller絶対期限をCWTで延長しない。
  - **両adapterでの実測実績:**
    - byte_chunks/LRC代表 および complete_block/CRC代表 での timely I-block 受領・元APDU成功・次APDU遷移を確認。
    - 両adapterで同じ有効EDC block内の一gapが確定超過（下界>CWT）または境界跨ぎ（uncertain）のとき、空response・known=false・元I再送なしを確認。
    - 上界がCWTちょうどのLRC境界ケース（F372/D1/4MHz、既定CWI13 CWT=762879000ns、誤差2ns、raw gap=CWT-2ns、widened upper=CWT）を受理。
    - evidence欠落、clock domain不一致、octet count不一致の非成功失効を確認。
    - TB3=`0x90`（CWI=0、CWT=1116000ns）を用い、100ms gapがCWI=0 CWTを超過しCWI=13（762.88ms）とは異なる判定となることをcomplete_block/CRCで確認。
    - CWI 14（F=1024, D=1, 4MHz、CWT=4,197,120,000 ns）において 1秒 gap（1,000,000,000 ns）の timely I 受領・同一 APDU 完了・次 APDU 成功を確認。
    - CWI 15（F=1024, D=1, 4MHz、CWT=8,391,424,000 ns）において 6秒 gap（6,000,000,000 ns）の timely I 受領・同一 APDU 完了・次 APDU 成功を確認。6秒 gap は CWI 13（約 2.1秒）や CWI 14（約 4.2秒）を超過し CWI 15（約 8.4秒）でのみ timely となるため、CWI 15 の独立弁別証拠となる（F=372 の旧 CWT 762,879,000 ns と F=1024 を混同しない）。
    - 既存WTX/C3、overflow/caller deadline、BWT/R回復、IFSD/連鎖fixtureの回帰通過を確認。
- **公開カードAPDU送受信API接続（PR62、C11終段: 公開 `dtv_transmit_apdu`・CARD能力・error順序・Core所有応答byte寿命・同一caller期限/ticket/epoch・両adapter LRC/CRC代表実測・未測定境界と作者検証帰属）:**
  - **位置付けと公開境界:**
    - PR50〜PR61（単一I、R再送・系列回復、BGTガード、連鎖、IFS交渉、BWT/WTX判定、CWT判定）で整備されたCore私有T=1 APDU交換処理を、正本（`spec/dtv11-l3-spec.md` §§6.1, 6.2, 6.7, 9、`l3/dtv.h`）で規定された既定の公開C API `dtv_transmit_apdu` へ接続する段階である。
    - 入口引数・options検証、DTV_OP_CARD能力照会、カード状態（`CARD_READY`）、カードlease/operation ticketの直列化、既存reader/request/connection epoch照合、既存session sequence（$N(S)$ / $N(R)$）、およびcaller絶対期限と連携し、私有T=1 engine（`exchange_public_t1_apdu`）へ1 APDUを委託。
    - **第二のT=1状態機械、固定物理chunk長、新規公開API、または独自の新規APDU上限は導入していない。** 既存の公開C宣言、正本、製品SPI表/署名との互換性を完全に維持。
  - **応答byte所有権・寿命・エラー分類・並行規則:**
    - 成功時、カードから返却された完全応答INF（SW1/SW2を含む全バイト列）を各筐体ごとのCore内部所有領域（`enclosure->card_apdu_response`）へ保存し、そのポインタとサイズを `out_response` / `out_size` へ返却。SW1/SW2を隠蔽・別statusへ変換しない。
    - 返却された応答byteの寿命は正本（決定26、§6.7）に従い「次の同一筐体card関数（`dtv_get_card_atr`, `dtv_reset_card`, `dtv_transmit_apdu`）呼び出しまで」有効。同一筐体内の別receiver操作（`dtv_get_signal` 等）やストリーム読取によって破棄されない。
    - 失敗時・結果不明時は部分応答byteを公開せず、`out_response = nullptr`, `out_size = 0` とし既存エラーマッパーで分類。正本§6.1が固定する順序は「未オープン時の `DISCONNECTED` → 宣言上非対応時の `UNSUPPORTED` → 機種/ファーム未準備時の `NOT_READY` → 実行」までであり、引数・options検証（空commandや不正optionsに対する `INVALID_ARGUMENT`）、caller期限切れ（`TIMEOUT`）、待機中取消（`CANCELLED`）、カード状態・世代フェンス（世代0未挿入時の `NO_CARD`、抜去後の `CARD_REMOVED`）等の各エラーは既存契約および実装の各処理段階におけるエラーマッピングとして扱われ、仕様にない全体の相互固定優先順位を設けない。
    - 結果不明APDU（送信後未完、CWT超過等）のCoreによる自動replayは行わない。
    - 待機中も同一ticket/epochを維持しながら別receiverを進め、caller絶対期限を延長せず、1-worker ランタイムの共有INと排他制御を遵守。
  - **両adapterでの実測実績（作者検証）:**
    - byte_chunks/LRC代表 および complete_block/CRC代表 の両私有adapterにおいて、公開 `dtv_transmit_apdu` 経由で異なる2つのAPDU（それぞれ 11 22 90 00、33 44 90 00）を連続実行し、実command I-block OUT、妥当なカードI応答受信、正確なCore所有応答byte返却、および次APDU成功を確認。
    - 同一筐体内の別receiver `dtv_get_signal` 実行後も第1 APDUの応答byte列が正常に保持されることを確認。
    - 各種エラー境界での代表実測:
      - 宣言上CARD非対応モデル: I/Oなしで `DTV_ERROR_UNSUPPORTED`（出力null/zero、OUTなし）
      - カード未準備（`CARD_READY` 前）: I/Oなしで `DTV_ERROR_NOT_READY`（出力null/zero、OUTなし）
      - 空command（サイズ0）: I/Oなしで `DTV_ERROR_INVALID_ARGUMENT`（出力null/zero、OUTなし）
      - 1ms caller期限切れ: 送信前に `DTV_ERROR_TIMEOUT`（出力null/zero、OUTなし）
      - 合成BGT待機中cancel: 送信前に `DTV_ERROR_CANCELLED`（出力null/zero、OUTなし）
      - カード抜去後: `DTV_ERROR_CARD_REMOVED`（出力null/zero、追加OUTなし）
- **Core私有T=1: byte/chunk部分I応答のCWT停止と根拠付き無通信からの有界R回復（PR63、C11終段: 公開 `dtv_transmit_apdu`・byte/chunk部分I受信・CWT超過と無通信>max(CWT,BGT)の根拠付き判定・期待系列R実OUT・完全I再送受領・Core所有応答byte寿命・同一caller期限/ticket/epoch・作者直接実行exit 0・未測定境界と作者検証帰属）:**
  - **位置付けと私有境界:**
    - PR62（公開 `dtv_transmit_apdu` 接続）に続き、byte/chunk搬送においてカードI応答の一部だけを物理的に受信した状態（妥当LENを持ちEDC前で停止する不完全I応答）から、規格に従って安全に回復する段階である。
    - 実command I送信後、受信した実octet列と、同一clock domain・誤差上界・evidence ID・plan revision・reader epoch・transaction ID・connection/request epochに結び付いた連続無通信証拠を検証。
    - 受信途中の物理文字edge間隔に基づくCWT超過判定に加え、最後に観測した文字以降にcard/hostともに通信していない連続区間の下界が `> max(CWT, BGT)`（BGT guard: 22 ETU）を満たすことを根拠付きで確認した場合に限り、Coreが期待カード系列（$N(R)$ = `receive_sequence`）を要求する有界R-blockを実OUT。
    - カードから再送された完全なI-blockを受領し、元の公開 `dtv_transmit_apdu` 呼出および後続の次公開APDUが正常に成功。応答byteはCore内部所有領域（`card_apdu_response`）へ保存され、正本（決定26、§6.7）に従い「次の同一筐体card関数呼び出しまで」有効。
    - **第二のT=1状態機械、固定物理chunk長、新規公開API、元command I/APDUの自動replay、および途中自動S(RESYNCH)（正本§6.2維持・ユーザー判断A保留維持）は導入していない。** 既存の公開C宣言、正本、製品SPI表/署名との互換性を完全に維持。
    - direct raw block交換の既存契約（EDC欠損時 `DTV_INTERNAL_RESULT_USB_IO`）は維持され、新wait / CWT timeout判定は公開APDUパス（`retained_ticket != nullptr`）に厳密に限定。complete_block搬送の部分受信証明、LEN不正時の終端推定、受信容量32超へのhost IFS拡大、PC/SC、CLI/daemon接続等は本PRの対象外。
  - **代表値・対照値・エラー境界（作者検証）:**
    - 成功代表実測（fault 100）:
      - 4 octetの部分I応答受信後、CWT = 762,879,000 ns、quiet lower bound = 862,878,998 ns、BGT guard = 22,000,000 ns（22ms）を観測。
      - `quiet_lower_bound_ns (862,878,998 ns) > max(762,879,000 ns, 22,000,000 ns)` が成立し、Coreは元command Iを1回、期待カード系列のRを1回実OUT。
      - 完全I応答（INF/SW1/SW2: 11 22 90 00）を受領し、第1 APDU成功。続く第2 APDU（33 44 90 00）も成功を確認。
    - 対照実測（fault 101: BGT未超過対照）:
      - CWI=0等で BGT > CWT となる条件下において、CWT = 762,879,000 ns < quiet lower bound = 763,878,998 ns < BGT guard = 792,000,000 ns を観測。
      - CWTは超過しているがBGTを満たさないためR/Sを送らず待機。100ms caller deadline先行により `DTV_ERROR_TIMEOUT` を返却、空response、command I 1回送出・追加R送信なしを確認。
    - 待機中cancel実測（fault 101待機中）:
      - BGT待機中に `dtv_cancel_enclosure` を呼び出し、`DTV_ERROR_CANCELLED` 返却、空response、成功誤認なしを確認。
    - 実待機中close実測（実operation-clock CWT待機中close）:
      - 実system operation-clock CWT待機確立後に別スレッドから `dtv_close_enclosure`（1ms期限）を呼び出し、closeは `DTV_CLOSE_PENDING`（数値2）を返却。再開した実行中callerは固定正本§6.2に従い保持ticketのclose取消を確認して `DTV_ERROR_CANCELLED`（数値21）を返却。空response、送信済みcommand Iは1回（I_frames=1）、追加R frameなし（R_frames=0）、最終状態 `DTV_STATE_CLOSED`（数値1）を確認。保持ticketのない新規closed-slot呼出しに対する `DTV_ERROR_NOT_READY` は従来どおり維持。
    - R後結果不明実測（fault 102）:
      - R送信後にカード応答が欠損した場合、APIは `DTV_ERROR_TIMEOUT` 返却、空response、成功誤認なしを確認。
      - なお、歴史的な fixed 100ms 条件（Windows simulated transfer callback/operation scheduling overhead により CardByteRead bulk-OUT 完了待ち中に 100ms caller 絶対期限を消費して return 279 となった実測）に対し、fault 102 fixture の caller timeout のみを 1000ms へ調整（commit `37bb90c8`）した修正後条件において、製品コード・製品期限・I/R predicate・TIMEOUT/空 response 不変のまま Windows focused executable（exit 0）および Linux focused CTest（367.40秒）の通過を確認（歴史的な fixed 100ms 条件と修正後 1000ms を混同しない）。
    - 既存direct契約実測（fault 1）:
      - EDC欠損時、新waitへ突入せず既存契約 `DTV_INTERNAL_RESULT_USB_IO` を保持することを確認。
    - 切り分け経緯:
      - 取消fixtureのwarmup/BWT mode 9残留問題はfixture mode復元により解消。
      - direct fault 1のエラー回帰（TIMEOUTへの誤変化）は、新wait処理を公開APDU（`retained_ticket != nullptr`）へ限定することで既存 `USB_IO` へ復元・解消。
      - 実待機中closeで呼び出し側が `NOT_READY`（数値3）を返す規格不一致（後述の独立QA不合格履歴）は、保持ticket存在時にclose取消を確認して `CANCELLED` へ戻す限定修正（commit `7132159b`）により解消。初回build時のlink未定義getter（`dtv_test_get_operation_clock`）はfixture定義追加で解消。
      - Windows CI での return 279 不合格（a16eb3ad snapshot、push/PR 各 attempt 1・2 の計 4 失敗。Linux は両 event 成功）は、Windows 環境下で CardByteRead bulk-OUT 完了待ちに fixture 側の caller deadline（100ms）を消費し意図した CWT/R へ到達しない fixture 予算不足であることを sequence/monotonic trace で特定し、fault 102 fixture の caller timeout のみ 1000ms へ調整（commit `37bb90c8`）して解消。製品の絶対期限、TIMEOUT/空 response、I/R predicate は不変（初回 public CI 4 失敗履歴は保持、修正後全 CI は未実施）。
- **後続工程との境界:**
  - 本PR（PR66）で接続されたのは、公開カードリセット（`dtv_reset_card`）、取得済み ATR の公開 getter（`dtv_get_card_atr`）、三層機種dispatch、合成reset要求・応答、Core共通ATR構造/TCK検査、返却領域所有・寿命、直列化・二重世代フェンス、決定51整合、L3 §6.2 に基づく PENDING close 停止回収、型付き私有カード搬送境界（PR48）、共通T=1完成block交換口（PR49）、Core内部単一I-block APDU交換・session sequence（PR50）、受信R-block再送と単一I-block再送制御（PR51）、受信I系列不一致に対するCore送信R-blockと系列回復（PR52）、Core私有BGT契約と送信前ガード（PR53）、カード側T=1 response chainingと正常R ACK（PR55）、Core私有T=1 command chainingと正常受信R ACK（PR56）、Core私有カード起点S(IFS)と端末S(IFS response)（PR57）、Core私有host起点S(IFS)によるIFSD縮小交渉と応答連鎖（PR58）、Core私有根拠付きBWT判定とI後タイムアウトのR回復（PR59）、Core私有カード起点S(WTX)確認と倍率付きBWT後のI応答（PR60）、Core私有完成応答blockのCWT判定と通常I応答（PR61）、公開カードAPDU送受信API接続（PR62: 既定公開 `dtv_transmit_apdu`、CARD能力・error順序、Core所有応答byte寿命、同一caller期限/ticket/epoch、両adapter LRC/CRC代表実測）、Core私有T=1 byte/chunk部分I応答のCWT停止と根拠付き無通信からの有界R回復（PR63: byte/chunk部分I応答、CWT超過と無通信>max(CWT,BGT)の根拠付き判定、期待系列R実OUT、完全I再送受領、Core所有応答byte寿命、同一caller期限/ticket/epoch、既存direct契約維持）、公開LNB on/off正常往復（PR65: `dtv_lnb_on`, `dtv_lnb_off`, `dtv_get_lnb_state`、既存モデルcallback・共通plan_operation・筐体ticket/共有USBによる合成OUT/相関ACK検証、根拠付きON/OFF、送信後不明結果UNKNOWN、無I/O未準備時の以前確認状態保持、直接L3給電許可）、公開ファームウェア転送（PR66: `dtv_send_firmware`、同一USB接続session、PublicDiscoveryState scan_only caller-pumped同期送信、既存OpenSlot worker/owner handle index再利用、短命ProductInstance/contextによる対象選択と寿命管理、same-handle runtime identify後の同一Device objectポインタ維持・mode RUNTIME更新・receiver readiness、同VID:PID loader/runtime重複機器に対する通常/token listでの製品bootstrap同期観測［1000ms共通絶対期限、同一公開device ID、既所有接続skip、肯定的identifyおよびclose成功時のみLOADER/RUNTIME確定、曖昧・失敗時UNKNOWN保持、同一objectポインタ維持・mode保持、元UNKNOWN discovery期待値維持、Core内容検証なし、公開open必須なし、公開L3不変］、UNKNOWN検出時無I/O NOT_READY、公開API/model callback署名維持）、および先行して main へ merge された Core 内部 wire フレーム外郭受送信（PR67: `dtv::internal::wire`、UDTV 1.0 magic 4byte `UDTV`、LE version 1、LE payload length上限1048504byte、flags/sequence/opcode/request_id外郭解析、FrameWriter/FrameReceiver chunk受送信、payloadゼロコピー/安全コピー、無効magic/version不整合/長過大/切り詰め検出、FrameReceiver::resetによる再同期受容、独立契約テスト）である。
  - 不完全blockのLEN不正時終端推定、complete_block搬送の物理部分受信証明、受信容量32超へのhost IFS拡大、その他S(ABORT/RESYNCH)等の規格内回復、途中自動S(RESYNCH)（保留中）、PC/SC、製品実機固有の電源/reset/card transport/コマンド、実USB機器、実機給電・電圧測定、package内容/manifest/hash検証（Coreはnull/size/bounds/device/owner/state/deadlineのみ確認しpackage形式・manifest・hashは要求しない）、既定FW自動選択/loader既定FW取得、to_all一括送信、USB再列挙（re-enumeration）、daemon許可gate・FD再許可、Android ランタイム実機動作、wire opcode固有payload解釈（個別codecは後続PR68/PR69/未決errorで実装）、wireソケット/パイプ/IPC/daemon接続、実機TS/CARD/TUNER wire中継、および公開L3 wire APIは本PRの対象外であり後続工程である。今回T=1規格全体準拠や全カード機能・全電源完成・全ファームウェア転送完成・wire API完成とはしない。

## CLI接続・既存レガシーとの境界・未実装範囲

- **CLI接続:** 生成された main は、argc/argv を変更せずそのまま既存Coreの `dtv_cli_main` へ渡す。L1の全parserや全subcommandの実装が完了したわけではない。
- **レガシーとの境界:** 既存のメタデータAPI（`dtv_model_count`, `dtv_model_get`）や旧内部ランタイム（USB、discovery、firmware転送）は残存しているが、本PRで導入された新L3 registryとは分離されている。正本との識別子衝突を回避するため、内部レガシー型・状態名は機械的に改名されている。また、旧来の `UsbContext` 既定コンストラクタによる2-worker常駐経路もレガシー境界として残存している。
- **未実装・未接続範囲:**
  - 製品実機の製品固有open計画・固有identify・初回/継続共有INの実機パラメータ・分類・実機CARD/LOCK通知値・実機受信部宣言・実機ロック判定閾値・実機tuneコマンド/機器丸め/RF変換・実機ストリーム転送/start-stopコマンド/ハードウェア通知フェンス・実機固有PID機器設定コマンド・実機固有電源/ファームウェア/reset transport/コマンド。
  - daemon / wire API（typed payload / HELLO / JSON / IPC transport / daemon host runtime / 5秒期限判定 / 結合は後続）、各製品リポジトリの実装接続。
  - CLI channel / ts / card / firmware 実行の完成。
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
    - PR49（共通T=1完成block交換および代表並行fixture）fixture切り分けおよび修正経緯:
      - 開発中のfixture実行において、fixture側の入力分割状態、1-byte末尾read仮定、transaction ID再利用、合成CRC ATRのTC配置、LRC期待値、close fixtureの保持session EDC期待値の誤りを合意済みwire/session条件に合わせて修正。
      - C7/C8代表fixture（commit `df95b45c`）実行において、初回はsetupで残ったUSB_DISCONNECTEDイベントを誤認していたためinterleave前にsetupイベントを排出するよう修正。ABSENT通知時の実測内部結果（`card_transport_context` の NOT_READY 返却）にアサーションを整合させ、後続close fixtureが前提とするCRC sessionを再確立するよう修正。実装不具合を隠すskipや期待緩和ではない（合成環境・fixtureでの修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR50（単一I-block APDU交換およびsession sequence）fixture切り分けおよび修正経緯:
      - scan_modelへcommand依存の独立vectorとwrong-sequence/R/S/chaining応答を追加し、既存fixtureを拡張。
      - 初回テストでbyte_chunks送信パケット数を3固定とした期待が失敗したが、3つのblock-writeに加えて6つのread-controlパケットが含まれることを切り分け、frameの3 writeを検査しつつread-control出力を許容する期待へ修正（製品期待値の緩和やskipなし。合成環境・fixtureでの修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR51（受信R-block再送と単一I-block再送制御）fixture切り分けおよび修正経緯:
      - 開発中の return 34 は通常の次 APDU を再送と同じ 2 frame と数えた test helper 期待誤りであり、期待 frame 数を引数化して切り分け。
      - return 33 は complete-block fixture が plan probe 時に一回限りの R 応答を消費し実転送時に I 応答を返していた fixture 状態管理が原因だったため、probe から実転送まで生成 R を保持する fixture 同期を追加。
      - sequence=1 を使う return 45 で R N(R) の bit mask を誤って 0x20 と扱う実装不具合が判明し、ISO PCB layout の bit 5 = 0x10 に修正し N(R)=1 再送 case を追加して解消。製品期待値の弱化や skip はない（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR52（重複I応答に対するCore送信Rと系列回復）fixture切り分けおよび修正経緯:
      - 初回 focused run で既存 fixture の即時 USB_IO 期待により return 31 となったため、R を含む新しい系列回復契約と照合して上限失効 UNSUPPORTED へ期待を修正。
      - 次の run で I(1) を I として認識する mask を bit 7 だけに限定していた実装誤りが tx/rx progress で判明し、I PCB の bit 8=0 を判定する mask へ修正。
      - 後続 run で response byte が正しく 33 44 90 00 であることを確認し frame 実数に合わせて rx 期待値を 1 block = 8 bytes へ修正。
      - complete_block helper が R frame 併存時に全出力を command frame 扱いしていたため論理 I/R frame をそれぞれ独立に数える helper へ修正（製品期待値の緩和や skip なし。合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR53（Core私有BGT契約と送信前ガード、実送信plan修正）fixture切り分けおよび修正経緯:
      - 開発中は構文ミスを修正し、focused test で fixture が上界状態を誤って保持していた問題と、合成 clock を system caller deadline へ写像していなかった問題を切り分けて修正。overflow 検査で不正 capability が reader state を汚染しないよう、checked arithmetic 完了後に状態を更新する制御へ修正。
      - 待機中revision改定修正において、仮想時計と実時計の期限領域のずれ、追加送信による既存APDU系列fixture状態変化、同revision不整合ケースのrevision誤りを切り分けて解消。timing-only再照会による期限延長・縮小時短縮なし・caller deadline超過・不整合拒否を確認。
      - 実送信plan一貫性修正において、deadline vectorがbyte_chunksを使い意図した通常CardT1BlockExchange plan overrideが発生しなかったfixture routeの選択ミスをcomplete_block routeへ切り替えて解消（製品期待値の緩和や skip なし。合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR55（カード側response chainingと正常R ACK、APDU既知性修正）fixture切り分けおよび修正経緯:
      - 途中のfocused実行で、実装がI-blockのN(S) bitをblock class bitと誤認した点を修正。
      - fixtureでは応答のLEN byte不足・fallback応答の重複生成・前blockのread offset残留を修正。
      - 途中I回復のresult_known変更は既存の初回重複I契約が維持される位置へ限定。
      - CRC経路のtx期待値を13から14へ訂正したのは、CRC付き9-byte commandと5-byte R blockの実wire長合計に合わせたためであり、製品期待値の緩和やskipではない。
      - QA所見（成果 `4eb1aaae`）に基づき、command送信後・M=1受信後の正常R ACK BGT待機中cancelにおいて累積tx非0・未完にもかかわらずknown=trueとなっていた既知性伝播不具合を特定。`exchange_public_t1_apdu` 共通finishで累積tx非0かつ未完なら `result_known = false` とする修正（+2行）を実施。送信前cancelのknown=true対照を維持し、送信済み未完の誤sequence・重複I/R retry exhaustion testのknown期待値をfalseへ訂正。新test後のresetで後続close fixtureのCRC条件が失われないようCRC再設定を追加（合成環境・fixtureでの修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR56（Core私有T=1 command chainingと正常受信R ACK）fixture切り分けおよび修正経緯:
      - 旧oversized_apdu fixtureの33 byte拒否期待を、IFSC超分割送信スコープに合わせて空command境界（長さ0でINVALID_ARGUMENT）の確認へ変更（65 byte/3片往復は別途専用fixtureで検証。製品期待値の緩和やskipではない）。
      - 途中片試験において、N(S) bit 6とR N(R) bit 4のwire bit位置の対応誤りを修正。
      - 受信byte期待値を完全block数（正常時R×2+応答Iで16 bytes、同N(R)再送時R×3+応答Iで20 bytes）へ修正。
      - 新fault 26のR drop状態が既存fault 15試験へ残らないようfixture切替時のdrop flag resetを追加。
      - BGT revisionを既存値18に維持してNOT_READYを解消（合成環境・fixtureでの修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR57（Core私有カード起点S(IFS)と端末S(IFS response)）fixture切り分けおよび修正経緯:
      - 初期ビルドで `next_ifsc` の宣言スコープが S ハンドラ内と狭く後続 APDU スコープから参照できなかった点を、APDU 交換スコープへ移動して解消。
      - 既存最終 command block 検査が C1 を UNSUPPORTED/USB_IO と判定していたため、最終 command 時の C1 のみを専用の検証および S ハンドラへ通すよう更新（中間片での S 要求は拒否を維持）。
      - 初期 22 ETU fixture において、byte_chunks 時に第 2 OUT 転送が必ずしも E1 フレームとならない問題を解消するため、実際の E1 ヘッダ/完全フレームを特定し、その OUT 提出時刻と最初の完了 IN 時刻を比較するよう修正。
      - 初期 ACK アサーションが Core OUT から R フレームを探していた誤りを解消。R ACK はカード応答であるため、モデル側で生成された R N(R) 系列を記録し、テストで R(0), R(1) をアサートするよう修正。
      - 初回 EDC テストで既存グローバル fault 設定が complete-block プローブ経路を破壊しなかったため、タスクローカルな malformed-EDC fixture モードを追加し byte_chunks 経由で E1 送信前に Core が拒否することを確認。
      - missing-I fixture での assertion abort が LEN 設定前の応答バイト未構築に起因していたため、モデル側で有効な I フレームを構築しつつ配送をドロップするよう修正し、Core のタイムアウト経路へ正しく到達することを確認。
      - 初回 cancel fixture で応答処理が先行レースしていたため、S-request IN 完了直後の合成 BGT 遅延中に cancel を発火させるよう修正し、CANCELLED と S 応答 OUT なしを確認。
      - fixture abort 切り分けのため一時的に gdb のインストールを試行したが apt lock 権限により失敗したため、一時的な backtrace handler によりアサーション箇所を特定・修正し、最終検証前に backtrace handler を削除（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR58（Core私有host起点S(IFS)と根拠付きIFSD縮小）fixture切り分けおよび修正経緯:
      - 初回ビルドで plan receive_capacity 比較の位置が plan 宣言より前になっていたコンパイルエラーを解消し、実 plan callback 後・物理 operation 前へ移してビルド成功。
      - CRC 成功 fixture の初期期待 rx=38 に対し、実バイト内訳が E1 6 + I(0,1) 21 + I(1,0) 9 = 36 であったため、観測フレーム長および ISO prologue/EDC 算術に合わせて期待値を 36 へ訂正（実装不具合の隠蔽や skip ではない）。
      - complete_block 受信経路で EDC 検査が未実施（破損 E1 を受理してしまう不具合）であった実装漏れを fixture で再現特定し、Core 共通の EDC 検査を有効化して破損応答を確実に拒否する回帰を追加。
      - LEN=2 の不正 E1 は要求上限の complete-block バッファより大きく、受信 byte 数 0 の段階で拒否され非成功コードとなったため、期待判定を成功可否・knownness・累積 tx/rx・部分 response/no-command で判定するよう限定し、不正 LEN 自体が拒否されることを保持（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR59（Core私有根拠付きBWT判定とI後タイムアウトのR回復）fixture切り分けおよび修正経緯:
      - 追加 fixture において mode4 capability が 4MHz のままであったため 204 エラーとなった問題を、`card_clock_hz = 0` に修正して解消（BWT 実装不具合ではなく unknown-clock fixture 条件の誤り。期待値変更や skip なし）。
      - generic USB read timeout に BWT 未証明判定を誤適用する実装不具合を検出し、同じ transport error を維持するよう修正して成功。
      - 初回独立 QA（成果 `21d6df6e`、head `7c5f4509`）において、2-I card response chain 後の host 次送信 1 / カード期待受信 0 状態で次 APDU の BWT absence が発生した際、Core が誤って $R(N(R)=1)$ を送出する反例プローブが検出され不合格。これに対し、Core の BWT 回復 R 生成における PCB NR 計算を `next_send_sequence` から `receive_sequence` へ変更し、期待カード受信系列 $N(R)=0$ を送出するよう修正（作者 commit `6ec97510`）。
      - 系列分離回帰テストの初回実行時、既存 fault 0 fixture モデルにおいてカード応答系列が host 要求系列へ誤結合していたため code 7 / 空応答が発生。これを独立したカード応答系列を持つ mode 50 へ分離し、期待 payload を fixture 仕様の 11 22 へ調整して成功させた経緯を仕様条件と分けて記録。
      - 修正後独立 QA（成果 `4b6667f2`、head `572db453`）において、両 adapter で独立系列 $R(N(R)=0)$ 送信・期待 I(0) 受理・同一/次 APDU 成功、全 host 24 CTest、NDK r27c static build、quiet-shelf public CI の成功を確認した一方、実 R PCB `0x82` の low nibble（`0010`: other errors）に対し、文書記述が `0000`（error-free）と説明する分類不一致を検出して不合格判定。文書側で NR と R error field を区別し、規格分類に合わせて訂正（timeout を `0010` に結び付ける規格文言の強さは留保し、実装不適合の断定は行わない）。
      - 予約 BWI 専用 fixture は reset helper が 208 で ATR 評価前停止したため未判定（製品不具合と未確定）。JPlag は実行ファイルが見つからず未実行（後続 QA が環境確認/必要時取得）。
      - なお、元作者成果物 JSON の generic USB timeout 不具合記載と exit 0 の併存、および Issue 26: 6896626506 の初回 reset 待機失敗から再実行成功の対応は原ログ回収不能で未確定と記録されており、今回の NR/error field 不具合との因果を割り当てない（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR60（Core私有カード起点S(WTX)確認と倍率付きBWT後のI応答）fixture切り分けおよび修正経緯:
      - private block exchange ヘルパーのシグネチャ変更により発生したリンクエラーに対し、既存 8 引数シグネチャから新 9 引数実装への forwarding overload を追加し、既存 SPI/API シグネチャ互換性を維持。
      - CRC adapter テストにおいて command I フレームを LRC サイズで計上していた期待値誤りを、CRC を含むフレーム長に訂正。
      - 旧 fixture で無効扱い（fault 35）されていた有効 WTX 倍率 16 を正規動作として除外、倍率 0 を発行する専用 fault 53 を追加。
      - byte_chunks adapter における転送パケット数前提の誤りを、プロトコル条件（元 I 1 回、E3 0 回）へ訂正。
      - BWT mode1 fixture が OUT ack 時に判定を誤付与していた問題を、実際の IN receive callback（kind 4/5）限定へ修正。
      - delayed callback fixture の caller timeout デフォルト（1000ms）を 5000ms へ拡大（WTX は caller 期限を延長しないため）。
      - CRC C3 + I 応答バイト数のループスコープ外参照コンパイルエラーをローカルスコープ計算へ修正。
      - WTX 呼出期限 fixture で BGT capability revision を適切に更新・復帰して TIMEOUT を正しく観測（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
      - WTX 倍率上限（旧 1..59）に関する親からの確認（Issue 25: 6901455194）に対し、ユーザーが推奨 A（1..255 受理・0 のみ拒否、caller 期限維持、積 overflow 失敗、正本不変、Issue 25: 6901870056）を採用したことを受け、`src/l3-registry.cpp` の 2 箇所から 59 上限キャップを除去。
      - 1 Hz 算術 overflow fixture において、初回 ATR の TA1 欠落による F=0 拒否（attempt 1）、TD1 の Y2=8 による TB3 未解析（attempt 2）、TD2 の T=0 指定による CRC 未適用（attempt 3）を順次識別し、正規の CRC T=1 ATR（`3B 90 B1 D1 01 00 F1 FE 90 01 01 6E`）へ修正して BWT 算術および積 overflow UNSUPPORTED 失効を正しく観測（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない。なお、以前の予約 BWI ATR probe 未到達を今回の ATR 不備と同因果と無条件に断定しない）。
    - PR61（Core私有完成応答blockのCWT判定と通常I応答）fixture切り分けおよび修正経緯:
      - complete_block fixture において、`scan_prepare_t1_reply` が transaction ID をリセットしていたため、reply 準備後に実 block transaction ID を設定し、BWT/CWT 証拠の transaction 対応を保持（attempt 2）。
      - byte_chunks の USB 出力スナップショットに送信 block 以外の read 要求（write 3 件 + read 3 件 = 6 件）が含まれていたため、誤った総 transfer 数前提を排し、I-block 送信回数と tx byte 数で再送なしを検証するよう修正（attempt 3）。
      - CWT fault 91 が既存 fixture の fault>=30 分岐と衝突していたため、CWT fault 注入を独立 atomic へ分離し、APDU 応答生成 fault を 0 に維持（attempt 4）。
      - 既存 delayed WTX ケースの直前に CRC を設定して reset し、LRC 状態の持ち越しを解消（attempt 5）。
      - byte_chunks において、evidence octet count 7 と LEN/EDC 確定 block 長 6 の不一致判定が余剰 octet 判定より先行していたため、余剰 octet 検出後に CWT を判定する順序へ修正。余剰データの既存 `USB_IO` 分類を維持しつつ CWT evidence 不整合は非成功（attempt 6）。仕様の期待を保ち誤った fixture 前提を修正（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
      - 独立 QA（成果 `a6cb4c6f`、head `a8849c3b`）において、有効 ATR（`3B 90 B1 D1 01 00 F1 FE 9F 01 01 61`、TB3=`0x9F`: BWI=9, CWI=15）のプローブを実行したところ、規格（ISO/IEC 7816-3 §11.4.3）に予約規定が存在しないにもかかわらず、実装が無根拠に `cwi > 13` を予約拒否（UNSUPPORTED）としていたため、最初の command I 送信前に return 249（`tx=0/rx=0`）で拒否され、不合格判定（failure 94）。これに対し作者コミット `fecc4ec2` により、`src/l3-registry.cpp` の ATR/session パラメータ検証および `t1_cwt_duration_ns` の両方で `cwi > 15`（4bit 範囲外）のみを拒否するよう修正し、TB3=`0x9E`（CWI=14、1秒 gap）および TB3=`0x9F`（CWI=15、6秒 gap、F=1024/D=1/4MHz）の synthetic fixture を追加して timely I 受領を確認・解消（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
    - PR62（公開APDU正常T=1往復）fixture切り分けおよび修正経緯:
      - 待機中 timeout fixture において、合成 BGT 状態が既知 mode/revision へ復旧されておらず初回 NOT_READY となった問題を、妥当な合成 revision 設定と既知 mode 復元により解消し TIMEOUT を確認。
      - セッションチケット処理において、shared operation pointer を後続参照前に move してしまい fixture クラッシュが発生した問題を、T=1 交換処理中に shared copy を保持するよう修正して解消。
      - fixture lambda の戻り値型推論不一致によるコンパイルエラーを、明示的な `int32_t` 戻り型指定により解消。
      - 初回 focused pass 後、カード抜去後の公開 API アサーション（`CARD_REMOVED`）を追加し、再ビルドおよび focused 再実行で完全成功を確認（合成環境・fixture での修正実績であり実機ハードウェア検証済みとは扱わない）。
      - なお、PR61 push Linux CI 初回で発生した IFS cancel 試験の return 160 失敗（同一 job 再実行で成功、根因未確定）の記録は過去の経緯としてそのまま保持し、PR62 検証へ流用しない。
  - PR47作者検証: 変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 pass（2.37秒。reset 前の getter NOT_READY、reset 後の getter バイト列・長さ完全一致、getter 単独での追加 endpoint 出力なし、reset 失敗後の出力クリアと getter NOT_READY、unsupported 筐体での出力クリア、別 receiver signal 進行後の pointer 有効性維持等を確認）、`git diff --check` 通過。
  - PR48作者検証: 変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 pass（2.47秒。既存CARD/receiver/close回帰に加え、byte write/readとcompleted T=1 block往復、送受信byte数、応答欠落時unknown/no-retry、in-flight closeのCANCELLEDとPENDING回収を確認）、全target build 成功（`cmake --build build --parallel 2`）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `e7f6694155cb90d1a0926736ded4d64f83f1ff5b`）。
  - PR49作者検証:
    - 旧head（`71bac0a5`）: 変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 pass（18.93秒。LRC/CRC、byte-chunkと完成block双方の往復、応答bytes/進行、不完全・余剰・不正frame、timeout/no-replay、closeを確認）、`git diff origin/main...HEAD --check` 通過。
    - 最新head（`df95b45c`）: source/header不変、tests/fixturesのみ更新（+296, -1行）。変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 連続2回 pass（20.94秒、20.99秒。保留中CARD_REMOVED通知・state ABSENT・generation進行・別receiver 192→188 read因果同期、遅延旧INF 6F00フェンス `tx=8, rx=0`・不明・追加OUTなし、新session INF 9000成功を確認）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `df95b45cfc23060210da204393e92adf329178f9`）。
    - なお、初回QAにおけるC7/C8判定不能はprobe配送/同期限界（外部probeからの同期観測限界）によるものであり、今回の作者fixtureは代表動的成功であるが、独立QAはこれから実施される。
  - PR50作者検証: 変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 pass（35.19秒。単一I/I往復、両adapter連続2往復、N(S)=0/1遷移、command別INF、IFSC超過無I/O拒否、誤sequence/R/S/chaining拒否・session無効化・reset後回復、保留中CARD通知/別receiver 188 read/遅延旧応答拒否/close取消を確認）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `e7f28b456e2e4fe6ff1c30155c8093b356994a53`、PR50 OPEN、base `t1-block-20261115`）。
  - PR51作者検証: 変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 pass（61.75秒。両adapterでの `I(0,0) -> R(0) -> I(0,0) -> response` および次 APDU での `I(1,0)` 遷移、complete-block での `N(S)=1` 時の `R(1) -> I(1,0)` 再送と sequence wrap、error indication 1 受理、不正 R・試行限度 3 回停止、初回到達前欠損での再送なし、R 受信後欠損での進行量保持と session 失効、既存 CARD 通知/別 receiver 188 read/遅延旧応答拒否/reset 回復/close 取消回帰を確認）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `f170d710aa74ce54770ea5f50eace1be0e2a4acb`、PR51 OPEN、base `t1-iblock-20261115`）。
  - PR52作者検証: 変更対象 `usb_claim_lease_contract` の build 成功、focused 1/1 pass（82.53秒。両adapterでの重複 `I(0,0)` に対する `R(1, other error) = 00 92 00 92` 送信・正しい `I(1,0)` 応答回収・重複 INF 破棄と正しい INF 返却・次 APDU での `I(0,0)` 遷移、complete-block での 3 回 R 試行上限 UNSUPPORTED 失効と reset 回復、R 送信後応答欠損時の 100ms caller deadline TIMEOUT・INF なし・`result_known=false` 失効、既存 CARD 通知/別 receiver 188 read/遅延旧応答拒否/reset 回復/close 取消回帰を確認）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `91bb24f596a3dfaf316be1071f489f2aa6f81c3a`、PR52 OPEN、base `main`）。
  - PR53作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（初回 90.82秒、待機中改定修正後 commit `3ec59498` で 100.52秒、実送信plan修正後 commit `87aa42f5fb24fff10b16d871af351cf1b53b9ee4` で 96.50秒。合成単調時計・期限直前のETU上界増加時の220ms待機延長・縮小時短縮なし・新期限超過時TIMEOUT・unknown/上界0/overflow/rollback拒否、重複I→R→正常Iの共通send入口通過、待機中cancel時のOUTなし・CANCELLED回収を確認）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `87aa42f5fb24fff10b16d871af351cf1b53b9ee4`、PR53 OPEN、base `main` `6bc7b12315ed4bbf9f4a6c7a7a9dd33ebeae97a2`）。全ターゲットビルド（`cmake --build ... --parallel 4`）、全24 CTest通過、Android NDK r27c static build、JPlag由来比較、およびquiet-shelf公開同一snapshot CI（Ubuntu 24/24、Windows 23/23）は独立技術QA（成果 `0e797dd4`）にて成功確認。
  - PR55作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（初回 108.95秒、既知性修正後 commit `c8eca733` で 114.92秒。byte_chunks/complete_block両adapterでの2 block連鎖・次APDU系列引き継ぎ、3 block代表連鎖、LRC/CRC、BGT実OUT境界22 ETU、M=1受信後R前cancelでのtx=8/rx=6/known=false/complete=false/OUT 1回のみ・no command replay、送信前cancelのtx=0/known=true対照、単発重複IのR回復継続、反復重複Iの3回上限known=false失効を確認）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `c8eca733f03de94b478345376cfe4499fe13449b`、PR55 OPEN、base `main` `9dd19f2aa4be0c9eaa86dcab76477bc3ac01594b`）。
  - PR56作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（131.38秒。両adapterでの65byte/3片送信・正常R ACK・同N(R)現在片再送対照・最終応答・次APDU遷移、BGT各command OUT間22 ETU下限、予約bit不正R・早期I・ACK欠損・部分OUT・待機中cancelでのknown=false/失効を確認）、`git diff --check` 通過、GitHub exact-head 照合一致（commit `5fd69f245e2adada81c8bd22464e654b3229f69d`、PR56 OPEN、base PR55 fixed head `6ff7c75d5554eb829e731736c6ee1ed6586601f9`）。
  - PR57作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（162.15秒、`ctest --test-dir build --output-on-failure -R '^usb_claim_lease_contract$'`。両adapterでの正常S(IFS)往復・新IFSC=16確定・次APDU 33byte commandの16/16/1片分割送信・R ACK受理、complete_block CRC代表での正常S(IFS)往復・同値E1 INF、合成IN完了→実E1 OUT境界22 ETU下限、不正IFS 00/FF・方向不正E1・LEN不正・予約PCB・EDC不正・partial OUT・BGT待機中cancel・後続I欠損でのknown=false/失効を確認）、`git diff --check` 通過（whitespace errorなし）、GitHub exact-head 照合一致（commit `3e142543d0cda64272a50bb1d3e0eec2c6985ae9`、PR57 OPEN、base `t1-command-chain-20261115` `83c97c25a1025610c28a1e87d2704b8bb73b4bf2`）。
  - PR58作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（198.80秒、`ctest --test-dir build -R '^usb_claim_lease_contract$' --output-on-failure`。両adapterでの正常S(C1, INF=10)→S(E1, INF=10)往復・新IFSD=16確定・16+4 response chainによる20byte APDU応答・正常R ACK・次APDU I(1,0)遷移、complete_block CRC代表での正常往復（tx=20, rx=36）、合成IN完了→実OUT境界22 ETU下限（S response IN→command I実OUT、chained I IN→R実OUT）、unknown/32以上容量でのSなし、不正容量・IFS 00/FF・値違い/方向違い/不正LEN/EDC/NAD・partial OUT・missing response・cancel・IFSD超過でのcommand未送信時known=trueおよびcommand送信後known=false失効を確認）、`git diff --check` 通過（whitespace errorなし）、GitHub exact-head 照合一致（commit `0e7c77020e398d554fe100e5586a362a2c1408f0`、PR58 OPEN、base PR57 fixed head `615f487497faa3ba7945620059a4d4c08d01dac7`）。
  - PR59作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（初回 241.71秒、NR 修正後 commit `6ec97510` で 252.06秒、`ctest --test-dir build -R '^usb_claim_lease_contract$' --output-on-failure`。両adapterでの根拠付きBWI（4）・有効clock（4MHz）正常I/I往復・BWT内最初文字edge判定・完全block受領・次APDU遷移、BWT境界直前edge（1ms）およびcallback遅延（20ms）時の正常timely判定、確定absenceによるI→BWT超過→R→I回復・有界3回再試行・次APDU遷移、送信I系列と期待受信I系列が独立した2-I response chain後のBWT absenceに対する $R(N(R)=0)$ 回復・元I再送なし・APDU完了・次APDU遷移、clock不明（0）・clock domain不明・誤差上界不明・境界跨ぎ誤差での拒否/uncertain、回復後known=trueおよび元I再送なしを確認）、`git diff --check` 通過（whitespace errorなし）、GitHub exact-head 照合一致（commit `6ec97510f94c7a15271b1ddd6d009d6b7a615995`、PR59 OPEN、base `main` `db452733a8b3a95422471c9c1544be1dd06bd92a`）。
  - PR60作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（初回 288.58秒、倍率 1..255 / overflow 修正後 commit `6373be4f` で 305.19秒、`ctest --test-dir build -R '^usb_claim_lease_contract$' --output-on-failure`。両adapterでの正常I→S(C3, 倍率2, 60, 255)→S(E3, 同倍率)→延長BWT内カードI(0,0)受領・同一APDU完了・次APDU遷移、積 overflow（BWT * 101 > UINT64_MAX）での UNSUPPORTED・E3なし失効、反復WTX、callback遅延1500ms、延長BWT境界直前edge受領、真正延長超過時の規格R回復・元I再送なし、倍率0拒否、部分E3 OUT、E3送出前cancel、request epoch不一致、caller期限切れTIMEOUTでの失効・knownnessを確認）、`git diff --check` 通過（whitespace errorなし）、GitHub exact-head 照合一致（commit `6373be4f72416e047702a1ba0177b4f97155f291`、PR60 OPEN、base `main` `be01c60dace45a1a3136d3d52a4cddd37efd1ee4`）。
  - PR61作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（初回 334.10秒、CWI 0..15 許容範囲修正後 commit `fecc4ec2` で 340.06秒、`ctest --test-dir build -R '^usb_claim_lease_contract$' --output-on-failure`。byte_chunks/LRC代表およびcomplete_block/CRC代表でのtimely完成I受領・元APDU完了・次APDU遷移、CWI 14/15 有効 ATR（TB3=`0x9E`/`0x9F`）受領と 1秒/6秒 gap timely 受領、1 gap超過・境界跨ぎuncertain・evidence欠落/domain不一致/count不一致での空response・known=false・元I再送なし、上界ちょうどCWTでのtimely受理、TB3=0x90 CWI=0 CWTでの100ms gap超過discriminator、既存WTX/overflow/caller deadline/BWT/R回復/IFSD/連鎖回帰を確認）、`git diff --check` 通過（whitespace errorなし）、GitHub exact-head 照合一致（commit `fecc4ec244b36f93d886c624e27ca6e66dfdaf82`、PR61 OPEN、base `main` `0566974c3cecd789db6a88c023f5c643bf519108`）。
  - PR62作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、focused 1/1 pass（352.27秒、`ctest --test-dir build -R '^usb_claim_lease_contract$' --output-on-failure`。byte_chunks/LRC代表およびcomplete_block/CRC代表での公開 `dtv_transmit_apdu` 経由2連続APDU成功・実OUT/系列/応答byte一致・次APDU遷移、同一筐体別receiver signal後の応答byte保持、CARD非対応UNSUPPORTED・未準備NOT_READY・空command INVALID_ARGUMENT・1ms期限TIMEOUT・BGT待機cancel CANCELLED・カード抜去CARD_REMOVEDの各出力null/zero・無追加OUTを確認）、`git diff --check` 通過（whitespace errorなし）、GitHub exact-head 照合一致（commit `9b9ff53033291d21a708e0785c87703ace919db3`、PR62 OPEN、base `main` `950598b789ff7d09f4e7893df896bb92bc73b971`）。
  - PR63作者検証: 変更対象 `usb_claim_lease_contract` の build 成功（`cmake --build build --target usb_claim_lease_contract -j2`）、Linux focused CTest 1/1 pass（367.40秒、`ctest --test-dir build -R ^usb_claim_lease_contract$ --output-on-failure`）および Windows focused executable 直接実行 exit 0（VS 18 / MSVC 14.51。byte_chunksでの妥当LEN不完全I応答からのCWT超過・無通信>max(CWT,BGT)根拠付き判定・期待系列R実OUT・完全I再送受領・元APDU/次APDU成功、BGT未超過対照でのR未送出・期限切れTIMEOUT空response、待機中cancelでのCANCELLED空response、実operation-clock CWT待機中closeでのPENDING/CANCELLED空response/I_frames=1/R_frames=0/最終CLOSED、R後応答欠損でのTIMEOUT空response、既存direct raw block EDC欠損でのUSB_IO維持、およびWTX等の既存fixture通過を確認。変更前focused CTest 352.29秒と直接実行最終runを混同しない）、`git diff --check` 通過（whitespace errorなし）、GitHub exact-head 照合一致（commit `37bb90c8830a7527b31288cb616c7a4fbbc2e3b7`、PR63 OPEN、base `main` `e66b62a71c07c6b6c633864d4d0096fe659ac814`）。
  - PR65作者検証: Coreルート構成からの fresh configure（`cmake -S . -B build -DBUILD_TESTING=ON`、libusb 1.0.23 wrapper check、configure、generate成功）、対象ターゲットの build（`cmake --build build --target usb_claim_lease_contract -j2`、`dtv-userland`, `scanfixture`, `usb_claim_lease_contract` build成功）、focused fixture 直接実行（`./build/tests/fixture/usb_claim_lease_contract --lnb`、exit 0。宣言YESの公開on→off正常往復: 実合成USB OUTと製品fixture ACK検証後にOK/状態UNKNOWN→ON→OFF、宣言NO: UNSUPPORTED/USB OUTなし、plan未準備: NOT_READY/USB OUTなし/以前確認済み状態保持、OUT送信後timeout: TIMEOUT/状態UNKNOWN/再送なし、close後の状態getter: UNKNOWN を確認）、`git diff --check` 通過（whitespace errorなし）、GitHub exact-head 照合一致（commit `628b32662c7086a4019ba50c065fe19747a593da`、PR65 OPEN、base `main` `c58ba42debcdd39828d5e4d9272ff1aef0440f7a`）。広い既存回帰（全fixture実行）は中断・未完了（PASSと報告しない）。
  - PR66作者検証: 初期実装（commit `aaf31ffd6699f9d7ce1b98aa4969e27900eecc75`）にて all configured targets build 成功（`TMPDIR=/home/yunomin61/dtv11-work/.tmp-public-firmware cmake --build /home/yunomin61/dtv11-work/.build-public-firmware -j2`）、focused 6 件直接実行 exit 0（`/home/yunomin61/dtv11-work/.build-public-firmware/tests/fixture/usb_claim_lease_contract --firmware`、`--firmware-unknown`、`--card-events`、buildroot直下 `usb_owner_contract`、`usb_async_io_contract`、`firmware_preflight_contract`。未open/open済み双方での同一USB接続session/caller-pumped送信・ACK・same-handle runtime identify後の同一object receiver readiness・2台目device/functionルーティング・UNKNOWN無I/O NOT_READY・署名不変を確認）、`git diff --check` 通過を確認。その後、独立 Windows QA 指摘に基づき fixture 宣言型不一致を修正（commit `94e4e4c2d10ea82d3bab374db6abc5d8664b37f0`: `tests/fixture/usb-claim-lease.cpp` の `scan_firmware_device_id` 宣言を `dtv_device_id` へ修正、製品コード不変）。さらに、同VID:PID descriptor重複機器に対する通常/token list製品bootstrap同期観測・mode確定修正（commit `eea368a86f381378edab846a7764a34b87a3e7e7`）を実施し、対象ビルド成功（`TMPDIR=/home/yunomin61/dtv11-work/.tmp-public-firmware cmake --build /home/yunomin61/dtv11-work/.build-public-firmware --target usb_claim_lease_contract discovery_public_contract -j2`、exit 0）、`discovery_public_contract` 直接実行 exit 0（元 UNKNOWN discovery 期待値維持）、`usb_claim_lease_contract --firmware` 直接実行 exit 0（same-ID 1234:0101 loader 識別→FW転送→RUNTIME観測、runtime 製品 no-op で FW OUT なし成功、既存 receiver support / 後続操作を確認）、`usb_claim_lease_contract --firmware-unknown` 直接実行 exit 0（真の UNKNOWN send 時 NOT_READY、identify および USB I/O なしを確認）、および Core `git diff --check` 通過（whitespace errorなし）を確認。全 host CTest suite、Windows 全件/直接実行、Android NDK r27c static build、JPlag 由来比較、および quiet-shelf 公開 CI は未実施（後続独立 QA へ委任）。
  - PR67作者検証: Coreルート構成からの fresh configure（`cmake -S . -B build -DBUILD_TESTING=ON`、成功）、all target build 成功（`TMPDIR=/var/tmp/opencode-wire cmake --build build -j2`、RC=0、warning 0件）、wire target build 成功（`TMPDIR=/var/tmp/opencode-wire cmake --build build -j2 --target wire_envelope_contract`、RC=0）、focused CTest 1/1 pass（`ctest --test-dir build -R wire_envelope_contract -V`、100% pass、0.01秒）、直接実行 exit 0（`./build/wire_envelope_contract`、RC=0。4 golden byte 列、旧 magic/版/kind 不正 12 件、上限および +1 境界 6 件、分割・連結・EOF・エラー後入力・出力不足・不正 view・1 MiB 上限の検証、test 側 setup 不備修正による code 20 解消を確認）、`git diff origin/main...HEAD` 照合一致（commit `5f33abbe46d58e54e5ed130caeb465d785e2a239`、PR67 OPEN、base `main` `3b86096694b89ed10e949cc8ee083e8667613afe`）。Linux 全回帰、Windows/MSVC、Android NDK static build、JPlag、および quiet-shelf 公開 CI は未実施（後続独立 QA へ委任）。
- **未実施事項・過去の不合格履歴:**
  - 過去の不合格履歴として以下を改ざんせず記録：
    - PR41修正前 head（`dd9220bb`）に対する独立プローブにより、親 close 時にブロック待機中の stream reader が解除されない不具合（failure 30 / exit 91）が検出・不合格判定された履歴（PR41修正により解消）。
    - PR53修正前 head（commit `8c2d1f8c7a0ed38a79e49c354ebbc78a9c8021ae`、作者コード `35ed4e2a`）に対する独立QA（成果 `0bf1d145a9da12c560914296a0fe160da01780c7`）において、待機中のETU上界改定（1ms→5ms）時に再照会を行わず旧期限で早期OUTする不具合（failure 62、88ms早期OUT）が検出・不合格判定された履歴（作者コミット `3ec59498` により修正）。
    - PR53待機中改定修正 head（commit `16478950ff958c72f3e251e084766c0f62f91b21`、作者コード `3ec59498`）に対する独立QA（成果 `00e8b810d5075574197f0649b07d499f692e088b`、`core-20261115-t1-bgt-revision-qa-result.json`）において、timing-only再照会後に取得された実送信 `CardT1BlockExchange` plan が新しいBGT capability（rev13/10ms）を返してもCoreがそれを無視して旧110ms期限でOUTする不具合（failure 63、exit 223、110ms早期OUT）が検出・不合格判定された履歴（作者コミット `87aa42f5` により修正、独立QA成果 `0e797dd4` により成功確認）。
    - PR59初回 head（commit `7c5f45096abd79d26df39be729a36d27281d0adf`、作者コード `55f7032c`）に対する独立QA（成果 `21d6df6e18cb000e27119be56fe9fa21bca7ceae`、`core-20261116-t1-bwt-qa-result.json`）において、送信I系列と期待受信I系列が異なる有効履歴（2-I card response chain 後に host 次送信 1 / カード期待受信 0 の状態で次 APDU の BWT absence 発生）で、BWT 回復 R が期待受信系列ではなく送信側次系列を $N(R)$ へ入れ、要求された I 応答を受けず APDU が失敗する反例（byte_chunks で $R(N(R)=1)$ 送信、exit 206）が検出・機能不合格判定され、後続の全 gate が停止された履歴（作者コミット `6ec97510` により $N(R)$ を `receive_sequence` へ修正、独立 mode 50 系列分離回帰テストを追加して解消）。
    - PR59修正後 head（commit `572db453f99faea878f45f238b5c65aeb100e4c0`、作者コード `6ec97510`）に対する独立QA（成果 `4b6667f2e7364c6988af31d1276f39e5cfd5ac6c`、`core-20261116-t1-bwt-r-sequence-qa-result.json`）において、実 R PCB `0x82` の low nibble（`0010`: other errors）に対し、文書記述が「bits 4..1=0000: エラーなし再送要求」と説明する規格分類不一致が検出・不合格判定された履歴（PR59 文書修正コミット `70c5f94a` により実 frame および規格分類 `0010` へ訂正）。
    - PR61初回 head（commit `a8849c3b17c2f6dff4ea9a0937a892784c24388e`、作者コード `9b9e759b`）に対する独立QA（成果 `a6cb4c6f8df5dffc745a58c58b1f7b9a93630112`、`core-20261116-t1-cwt-block-qa-result.json`）において、規格（ISO/IEC 7816-3 §11.4.3）に予約規定が存在しないにもかかわらず、実装が無根拠に `cwi > 13` を予約拒否（UNSUPPORTED）としていたため、有効 ATR（`3B 90 B1 D1 01 00 F1 FE 9F 01 01 61`、TB3=`0x9F`: BWI=9, CWI=15）のプローブで最初の command I 送信前に UNSUPPORTED（return 249, `tx=0/rx=0`）で拒否される反例（failure 94）が検出され不合格判定となった履歴（作者コミット `fecc4ec2` により CWI 許容範囲を 0..15 全域へ修正、CWI 14/15 fixture を追加して解消）。
    - PR62初回 head（commit `d2ada79e5bf289e62f5c579d119bdc044500419c`、作者コード `9b9ff530`）に対する独立QA（成果 `154e99b871d3885627991161936bfc88a4a7d23e`、`core-20261116-public-apdu-qa-result.json`）において、文書およびPR本文が正本§6.1の固定順序（DISCONNECTED→UNSUPPORTED→NOT_READY→実行）を超えて、正本が固定していない引数不正・期限切れ・取消・抜去までを含む全順序を正本に誤帰属させていた文書所見により不合格判定（`spec_priority_wording` FAIL）。指示書の重gate停止規則に従い全24件CTestは17件pass後に中断（shell終了130）、JPlagおよびquiet-shelf CIは未実行となった履歴を記録（PR62文書修正により解消）。なお、同QAにおいて、focused 1/1 pass（350.29秒）、世代0 ABSENT確認後の公開 `dtv_transmit_apdu` による `NO_CARD`（null/zero出力、追加OUTなし）、公開送信後CWT確定超過（fault 91: raw gap [CWT+3ns, CWT+3ns]）による `UNSUPPORTED`（数値4、完全なcommand I 1回送出、responseなし）および直後の別公開APDU `NOT_READY`（数値3、追加OUTなし）、全ターゲットbuild成功、公式NDK r27c static build成功が確認された（公開probeは確定超過であり不確定区間ではない。公開fault 93境界跨ぎ不確定は未実行であり、私有fixtureのfault 93 passを公開へ読み替えない）。
    - PR63初回 head（commit `3433a0d3c9e7bf3bd908e16b924dca16e4ccaa63`、作者コード `2848a5e1`）に対する独立QA（成果 `b325fc842ef6fff9e817fc93ba450f7723872cdb`、`core-20261116-t1-partial-cwt-final-qa-result.json`）において、実operation-clock CWT待機確立後のclose（1ms→PENDING）時に、固定正本§6.2が要求する `CANCELLED`（数値21）ではなく `NOT_READY`（数値3）を返す不一致が検出・機能不合格判定（`close_during_actual_partial_cwt_wait` FAIL、exit 148）され、重gate停止規則により全24件CTest/NDK/JPlag/公開CIが停止された履歴（作者コミット `7132159b` により保持ticket取消確認時に `CANCELLED` を返すよう修正して解消。なおQA rawログの `R=1` は「R frameが無い」真偽値でありR一回送信ではない）。また、初回 public CI（a16eb3ad snapshot）における 4 失敗（push 43857760884 / PR 43857772594 の Windows 各 attempt 1・2、すべて新規 return 279。Linux は両 event 成功）の履歴を保持し、修正後全 CI は未実施（新 279 は Windows fixture 100ms 期限切れ実測により一行修正 `37bb90c8` で解消、別件 return 27/160 調査の成果ではない）。
    - PR66初回 head（commit `aaf31ffd6699f9d7ce1b98aa4969e27900eecc75`、文書 `6ee0247aa3c279393248c5d6585b47e419b3f183`）に対する独立QA（成果 `81a586ce`、`core-20261117-public-firmware-qa-result.json`）において、Windows MSVC ビルドでの `scan_firmware_device_id` 戻り値型不一致（LNK2019/LNK1120）および Linux `discovery_public_contract` exit 5（descriptor-only 分類による同 VID:PID 0x1234:0x0101 の RUNTIME 誤判定と UNKNOWN アサーション失敗）が検出され overall FAIL となった履歴。旧 QA の一時 RUNTIME 期待値 patch（`87733c0b`）後の合格は採用版の受入ではなく、ユーザー（Issue 25: 6956510837）により元 UNKNOWN discovery 期待値変更の前提が否定され、同 VID:PID の製品観測による mode 修正が指示された履歴。作者成果 `core-20261118-public-firmware-mode-result.json` / `evidence` による Windows 型修正（commit `94e4e4c2`）および mode 本体修正（commit `eea368a8`）により解消。
  - 先行 PR53 はユーザー（Issue 25: 6890319472）承認により `main` へ merge 済み。先行 PR54（S自動再同期）は同ユーザー判断 A により未完了・未 merge で CLOSED（参照用branch `246a7ea6`）。
  - 先行 PR55 は `main` へ merge 済み（commit `c9fc7287c0eee2482fcb22db96942d2f32d23886`、API確認済み）。
  - 先行 PR56、PR57、PR58 はユーザー承認（Issue 25: 6897041415）により順に `main` へ merge 済み（PR56: commit `430dea2dcb0cdef9ddc57fcf5167564e53517080`, PR57: commit `3be2b16150bc704ffd3358d71b0fb28ad9805c1c`, PR58: commit `db452733a8b3a95422471c9c1544be1dd06bd92a`、API確認済み）。
  - 先行 PR59 はユーザー承認（Issue 25: 6900447467, 6901870056）により `main` へ merge 済み（commit `be01c60dace45a1a3136d3d52a4cddd37efd1ee4`、API確認済み）。
  - 先行 PR60 はユーザー承認（Issue 25: 6903062646、2026-11-15T23:19:17Z）により `main` へ merge 済み（commit `0566974c3cecd789db6a88c023f5c643bf519108`、API確認済み）。
  - 先行 PR61 はユーザー承認（Issue 25: 6905923496、2026-11-16T02:50:13Z、原文 `core-20261116-pr61-merge-user-decision.json`）により `main` へ merge 済み（commit `950598b789ff7d09f4e7893df896bb92bc73b971`、API確認済み）。
  - 先行 PR62 はユーザー承認（Issue 25: 6915949299、2026-11-16）により `main` へ merge 済み（commit `e66b62a71c07c6b6c633864d4d0096fe659ac814`、API確認済み）。
  - 先行 PR63 はユーザー承認（Issue 25、2026-11-16）により `main` へ merge 済み（commit `aae42794eb88773e3aeb6a13d33f20da62f7d983`、API確認済み）。
  - 先行 PR64（deadline cancel fixture 期限調整）はユーザー承認（Issue 25、2026-11-17）により `main` へ merge 済み（commit `c58ba42debcdd39828d5e4d9272ff1aef0440f7a`、API確認済み）。
  - 先行 PR65（公開 LNB 正常往復）はユーザー承認（Issue 25: 6951239857、2026-11-17）により `main` へ merge 済み（commit `3b86096694b89ed10e949cc8ee083e8667613afe`、API確認済み）。
  - 本 PR66 は `main`（`3b86096694b89ed10e949cc8ee083e8667613afe`）宛ての PR（branch `public-firmware-20261117`、source head `eea368a86f381378edab846a7764a34b87a3e7e7`）であり、OPEN、独立未受入・未 merge である。
  - **検証主体帰属の分離:** PR66 作者検証は、初期 build/focused 6件、Windows 型修正、および mode 修正後の対象 build（`usb_claim_lease_contract discovery_public_contract`、exit 0）、`discovery_public_contract`（exit 0）、`usb_claim_lease_contract --firmware`（exit 0）、`usb_claim_lease_contract --firmware-unknown`（exit 0）、Core `git diff --check` 通過のみである。全 host CTest suite、新 Windows 全件/直接実行、Android NDK r27c static build、JPlag 由来比較、新 quiet-shelf 公開 CI（Linux/Windows Configure, Build, CLI）、新 head 技術受入は後続独立 QA へ委任される（作者単独でこれらを合格済みと扱わず、過去 PR65 の成功を PR66 新 head の実測として扱わない。正本や API/SPI 署名不変という作者報告を「独立受入済み」とみなさない）。
  - **PR66 未実測事項・制限の明示:**
    - 合成 USB byte / identify / 公開観測成功は fixture 上の明示成功根拠であり、実機 FW 書込ではない。製品実機でのファームウェア書込および実機動作検証は未実施・未測定。
    - 全 device 実機での確認済みとはしない。
    - 1000ms 共通絶対期限、既所有接続 skip、同一 object と mode 保持等の制約を保持。
    - Core は package 内容/manifest/hash 検証を行わず（null/size/bounds/device/owner/state/deadline のみ確認）、公開 open も必須にしていない。公開 L3 API 署名は不変。
    - 未 open 既知 loader の到達性、真の UNKNOWN 無 I/O NOT_READY、2台目の正しい target/owner/context 寿命、同一 object からの後続公開 open / receiver 操作を受入条件として区別する。
    - descriptor で判定可能な fixture 等の作者実績を全製品対応や実機書込保証に広げない。初期製品（ASICEN/PIX/torne）、後続（PX4/Siano）等、不要な製品名列挙を発明しない。
    - 既定 FW 自動選択 / loader からの既定 FW 取得、to_all 一括送信、USB 再列挙（re-enumeration）、daemon / wire API の FD 再許可 gate、CLI / IPC は未実装・未接続のまま区別を保持。
    - 過去の編集前停止は履歴として残し、現実装済みと区別する。
    - 製品実機での固有電源/ファームウェア/LNB制御コマンド・実機切断/物理unplug時の挙動・Androidランタイム実機動作・性能保証・全製品完成は未測定・未検証。
    - 全ての deadline / 欠損応答 race の個別網羅、カード機能完成、ファームウェア転送完成、実機検証済みの断定は行わない。
  - 先行して main（`d9cb76670e650124eba72e0394596095a6a268bc`）へ merge された PR67（branch `wire-envelope-20261118`、head `2e3cf4e34bfcf5b48a1226824f7743564dce1bef`）の検証と境界:
  - **検証主体帰属の分離:** PR67 作者検証は configure 成功、all target build 成功、focused CTest 1/1 pass、直接実行 exit 0 のみである。Linux 全回帰、Windows/MSVC build、公式 Android NDK static build、JPlag 由来比較、quiet-shelf 公開 CI、新 head 技術受入は後続独立 QA へ委任される（作者単独でこれらを合格済みと扱わず、先行 PR65/PR66 の実績を PR67 新 head の実測として扱わない。また作者 result にある Emscripten 未実施を今回の新必須 gate に増やさない）。
  - **PR67 未実測事項・制限の明示:**
    - 実行経路は親 ssh → WSL であり、作者 evidence の `environment.host` における `HAOS SCS` 表記は誤記である（親指摘として区別、作者原 artifact は変更しない）。
    - 初回 `/tmp` 満杯 build raw を成功 log で上書きして全文欠落した限界を保持。
    - I/O・時計なし、5 秒期限は後続 host runtime の責務（FrameReceiver は時刻・タイマーを持たない）。
    - typed payload、HELLO、JSON、IPC transport、daemon 結合、CLI は未実装・未接続のまま区別を保持。
    - L2 未決（error payload 配置、STATE_EVENT 追加 feature bit、HELLO 上限、TS_DATA 0 packet）は未決定・未変更。
    - daemon / IPC 結合成功ではない。
    - 製品実機での固有電源/LNB制御コマンド・実機給電回路制御・実機切断/物理unplug時の挙動・Androidランタイム実機動作・性能保証・全製品完成は未測定・未検証。
    - 全ての deadline / 欠損応答 race の個別網羅、カード機能完成、wire API 完成、実機検証済みの断定は行わない。
    - 先行 PR62 の作者未測定事項のうち、世代0における `NO_CARD` および公開送信後CWT確定超過（fault 91）は独立QA一時probe（成果 `154e99b8`）にて実測されたが、別筐体card操作中の応答byte寿命保持、session待機中のclose競合、公開送信後CWT不確定（fault 93）時の挙動は依然として未実測であり、保証済みと扱わず後続QAへ未判定として引き継ぐ。
    - fixture で観測した文字 edge 時刻・callback 遅延・隣接 gap・CWI=0 discriminator（100ms）・CWI 14/15 gap（1秒/6秒）・公開APDU応答時間は合成データであり、実機 USB / card ハードウェアでの物理文字 edge 保証や実時間保証ではない。実機 adapter/firmware からの物理 gap 上下界・誤差上界供給と製品での実 CWT 保証は未測定。不完全 block の CWT lapse、block 終端判定、物理無通信（>max(CWT,BGT)）による送信権の取得、および安全な規格 R 回復は未接続・後続工程（正本 §6.2 / 途中自動 S 保留維持）。単なる計画時刻/前 OUT→次 OUT を IN 完了→実 OUT へ読み替えない。連鎖・S・公開APDU途中の個別 close PENDING / 旧 epoch 全組合せは未追加・未実測であり、成功を主張しない。また、合成単調時計での BGT/WTX/CWT 境界確認は実機物理保証の証拠ではなく、物理 BGT/CWT および実機・USB・HA・実カードとの通信は未測定である。受信容量32超への host IFS 拡大、欠損 block に対する途中自動 S 再同期（ユーザー判断 A により保留維持）はこの PR の成果に含めない。
  - 製品 adapter の plan callback 副作用や capacity-zero timing-only 対応、guard 待機中に connection/request epoch を更新する専用 fixture、guard 待機中に close を呼ぶ専用 PENDING fixture、guard 待機中の CARD 通知直接同時進行専用 fixture、境界ちょうどの独立 vector は未実測であり、成功を主張しない（BGT 待機中の別 receiver 188-byte read は独立 QA 成果 `0e797dd4` の一時合成 fixture probe にて実測済み、CARD 通知同時進行は未測定。tracked 専用 fixture の追加は行わない）。また、本合成 fixture は実機物理 BGT 保証の証拠ではなく、未確認な全製品の対応や任意瞬間の物理 clock 変化の安全性までを全面保証するものではない。
  - 先行 Windows の単発失敗原因（PR50 reset return 10、PR51/52 共通 timeout return 27）を同一原因と推測・断定しない（新しい全組合せ試験要求は作らない）。
  - **Android runtime の扱い:** Android runtime は後続の実機/runtime 工程であり、今回の QA には委任しない（今回 QA 委任は NDK compile まで）。
  - 全てのdeadline/欠損応答raceの専用個別網羅、T=1 全体準拠、公開 APDU 完成、ファームウェア転送完成、wire API 完成、実機検証済みの断定は行わない。
  - 実USB上の物理リソース衝突・物理unplug時の実機DISCONNECTED挙動、製品実機の固有PID機器設定コマンド・実機TS転送接続・実機固有電源/ファームウェア/reset transport/コマンド・実機カードAPDU・Androidランタイム実機動作・性能保証・全製品完成は未測定・未検証。
  - 製品実機のlock観測source/byte/閾値・校正CNR電力測定・実機tuneコマンド・実機ストリーム転送・機器丸め/RF変換は未接続・未設定（実値・性能SLAの推測なし）。card APDU、daemon IPC、CLI channel/ts/card/firmware 実行も未接続。
  - 実機 Android 端末・USB 機器での実機動作確認、実機切断、permission 受渡し動作確認は未実施。架空の実機成功や全 OS 保証は行わない。
  - 製品実機の初期化・実機エンドポイント値・実機CARD/LOCK通知値・カードAPDU・公開操作接続・同期途中中断保証や数値 SLA は保証しない。

## USB シリアル観測・機種導出 SPI・公開一意性契約（L3 決定 66）

正本 L3 §4.4、決定 66（正本 `e8ec8eb9`）、およびユーザー決定（`core-20261118-fourth-merges-serial66-user-decision.json`）に基づき、Core による USB STRING 記述子（serial）の観測、機種固有の筐体シリアル導出 SPI（`dtv_derive_enclosure_serial_fn`）、公開シリアル文字列規則、公開 getter（`dtv_get_enclosure_serial`, `dtv_get_serial_unique`, `dtv_get_device_serial`）、および一意性判定規則を導入した。

### 1. USB シリアル記述子の観測契約（Core 共通層）

- **STRING 記述子の復号と状態管理:**
  - Core は USB デバイスの STRING 記述子（UTF-16LE）を読み取り、可逆な lossless UTF-8 へ復号して保持する。
  - 状態（`dtv_usb_serial_state`）は以下の 4 区分として明確に管理される:
    - `DTV_USB_SERIAL_ABSENT` (0): デバイス記述子の `iSerialNumber` が 0（シリアル番号なし）。
    - `DTV_USB_SERIAL_PRESENT` (1): UTF-16LE 記述子を正常に復号完了。`serial_utf8`（0..378 byte）が有効。
    - `DTV_USB_SERIAL_UNREADABLE` (2): デバイス open 失敗、アクセス権限不足、BUSY、タイムアウト等による読取失敗。
    - `DTV_USB_SERIAL_MALFORMED` (3): 記述子長不正、種別不正、または不正なサロゲートペア等の異常。
- **観測範囲と制限:**
  - 観測対象は、ロード済み製品が宣言した VID:PID を持つデバイスのみに限定する。未宣言のデバイスに対する記述子読取は行わない。
  - 走査時に `claim_interface`、`detach_kernel_driver`、`reset_device` は一切行わない（アクティブなストリームや他のプロセスに影響を与えない）。
- **走査予算とキャッシュ（未測定試値）:**
  - 走査全体の予算は 2 秒、候補デバイスごとの予算は 250ms とする。候補ごとの 250ms は LANGID 取得と STRING 取得の 2 回の制御転送で共有される。
  - これらの数値（250ms / 2s）は合成検証用の未測定の試値であり、実機環境での確定 SLA ではない。
  - 同一の物理 USB 接続（connection token）において成功した復号 raw 値は Core 内部でキャッシュされ、一時的な読取失敗が生じても既知の値を破棄せず維持する。キャッシュは物理切断または再列挙（connection identity 変更）時に破棄される。
- **FD 経路・token 経路の所有権契約:**
  - 通常走査のほか、Android 等の FD 走査（`dtv_list_enclosures_by_fds`）および token 走査においても serial 観測が実行される。
  - FD 経路において、caller から渡されたファイルディスクリプタ（FD）の所有権は caller に留まり、Core が勝手に close したり所有ハンドルを破棄することはない。

### 2. 機種導出 SPI とモデル情報（`dtv::spi`）

- **候補情報への raw シリアル開示:**
  - `dtv_usb_candidate_info` に `serial_state`、`serial_utf8`、`serial_utf8_bytes` を追加。
  - `serial_utf8` は Core が復号した UTF-8 バイト列への借用ポインタであり、NUL 文字や制御文字を含みうる。`serial_state == DTV_USB_SERIAL_PRESENT` の場合のみ有効。コールバック期間中のみ有効な借用値である。
- **機種シリアル導出コールバック (`dtv_derive_enclosure_serial_fn`):**
  - `model_information` に `dtv_derive_enclosure_serial_fn derive_enclosure_serial = nullptr;` を追加。
  - コールバック署名:
    ```c
    typedef dtv_internal_result (*dtv_derive_enclosure_serial_fn)(
        const dtv_serial_derive_input *input,
        dtv_serial_derive_output *output,
        char *buffer,
        uint32_t capacity);
    ```
  - **純関数条件:**
    - コールバックは外部 I/O、スレッド生成、大域的・静的状態の変更を行ってはならない。入力情報のみに基づき決定論的に導出値を算出する純関数として実装される。
  - **2 段階呼び出しプロトコル（サイズ問い合わせと書き込み）:**
    1. **サイズ問い合わせ:** `buffer = nullptr`, `capacity = 0` で呼び出される。導出可能な場合、`output->outcome = DTV_SERIAL_DERIVE_VALUE` を設定し、`output->length` に必要なバイト数を返す。
    2. **書き込み:** Core が所有する 513 バイトのバッファ（`capacity = 513`）を渡して呼び出される。コールバックは結果を書き込み、`output->length` に実際の長さを返す。
  - **導出値の検証と失敗不変性:**
    - 導出されるシリアルの長さは 1..512 バイトでなければならず、513 バイト以上や 0 バイト、UINT32_MAX 等は拒否される。
    - 書き込まれた値は Core 側で `valid_public_serial`（strict UTF-8、NUL および制御文字なし）により再検証される。
    - コールバックが `DTV_SERIAL_DERIVE_UNKNOWN` を返した場合、エラーコードを返した場合、または検証に失敗した場合、Core は導出失敗（シリアル NULL）として扱い、出力先バッファの内容を変更せず、グループ判定（完全な筐体としての認識）自体を破棄・削除しない（グループを維持し、シリアルのみを NULL とする）。
- **未宣言時の既定動作（ハードウェア差の吸収）:**
  - 機種が `derive_enclosure_serial` を宣言しない（`nullptr`）場合:
    - 筐体に属する device が 1 つだけの場合: 当該 device の raw シリアルを採用する（公開規則検査を満たす場合のみ）。
    - 筐体に属する device が 2 つ以上の場合: 筐体シリアルは `nullptr`（NULL）とする。

### 3. 公開シリアル文字列規則と公開 API

- **公開シリアル文字列規則 (`valid_public_serial`):**
  - 公開されるすべてのシリアル番号（筐体シリアル、デバイスシリアル、孤立デバイスシリアル、および candidate シリアル）は、以下の条件をすべて満たす必要がある（正本 L3 §4.4 / 決定 66）:
    1. バイト長が **1 〜 512 バイト** の範囲内であること。
    2. 正しい（strict）UTF-8 文字列であること。
    3. **NUL 文字（`\0`）を含まない**こと。
    4. **制御文字（ASCII 0x01〜0x1F、0x7F 等）を含まない**こと。
  - raw 記述子に NUL や制御文字が含まれていた場合、raw 借用構造体（`dtv_usb_candidate_info`）には保持されうるが、公開 API（`dtv_get_*_serial`、`dtv_candidate::serial`、`dtv_ungrouped_device::serial`）からは NULL として扱われ、外部に露出しない。
- **公開 getter 関数:**
  - `const char *dtv_get_enclosure_serial(dtv_enclosure *object)`: 筐体シリアル文字列、または `NULL` を返す。
  - `const char *dtv_get_device_serial(dtv_device *object)`: USB デバイスのシリアル文字列、または `NULL` を返す。
  - `uint32_t dtv_get_serial_unique(dtv_enclosure *object)`: 一意性指標（`DTV_YES`, `DTV_NO`, `DTV_UNKNOWN`）を返す。
  - これらの getter は事前確保済みの内部状態を参照し、USB I/O や動的メモリ確保を行わない。

### 4. シリアル番号の一意性契約（`dtv_get_serial_unique`）

- **比較対象の集合:**
  - 同一走査内で検出された全製品の完全な筐体（complete group）を比較対象とする。
  - 一覧の最大容量（既定 128 筐体）を超過して一覧配列から省かれた完全な筐体も、比較対象に含まれる（省かれた筐体とシリアルが重複していれば NO となる）。
  - 孤立 USB デバイス（`ungrouped_usb_devices`）は一意性比較の対象外である。
- **一意性の判定基準:**
  - **`DTV_YES` (一意):**
    - 自筐体のシリアル番号が確定（known）しており、かつハードウェア走査が完了（`complete`）しており、同一走査内の他の完全な筐体とシリアルの重複が存在しない場合。
    - **他の筐体のシリアルが NULL または UNKNOWN であっても、自筐体の一意性（`DTV_YES`）を妨げない**（決定 66。シリアル不明な筐体はシリアル指定による選択対象とならないため。旧仕様の「他が未知なら YES 保留」は撤回・旧版仕様へ合致）。
  - **`DTV_NO` (重複あり):**
    - 同一走査内の別の完全な筐体と同一のシリアル番号を持つことが確認された場合。走査が未完了であっても、重複が検出された時点で直ちに `DTV_NO` となる。
  - **`DTV_UNKNOWN` (未確定・不明):**
    - 自筐体のシリアル番号が NULL の場合。
    - ハードウェア走査が未完了または失敗で終了した場合（ただし既知の重複がある場合を除く）。

### 5. 接点および未検証・未接続境界

- **合成検証の範囲:**
  - Core 内部の合成テスト（`tests/usb-serial.cpp`, `tests/fixture/serial-contract.cpp`）により、観測・復号・予算共有・cache・FD 所有権、SPI 導出プロトコル、公開規則検証、および一意性判定の契約を確認済みである。
- **「シリアル完成」「SPI 安定」の不成立:**
  - **Core 単体での合成テスト成功をもって「シリアル機能の完成」や「SPI 安定」とは呼ばない**（決定 66）。
  - 実際の PX-Q3U4 実機における 2 function raw 観測、14 桁 base シリアル導出、製品（px4-userland）側のコールバック宣言、および CLI / daemon による serial 機器選択動作は未確認・未接続である。
  - USB 記述子読取の実測遅延、実機 USB での権限・BUSY 挙動、およびアクティブな TS ストリームへの影響は未測定である。走査予算（250ms / 2s）は未測定の試値であり、実測に基づく見直しが後続課題として残されている。
  - 既存 CLI および daemon との接続は本 PR の対象外であり、未接続・未実装である。



## 正規runtime VID:PIDのグループ探索アンカー接続（PR84、内部基盤）

PR84 では、ファームウェア転送（cold boot）を持たない正規 runtime 宣言のみを持つ機器（`cold_identity_count == 0` かつ `cold_identities == nullptr`）を一般グループ探索（`observe_loaded_product_groups` / `collect_complete_groups`）へ接続する限定修正を導入している。

### 1. 概要とアンカー判定の拡張

- **目的と背景:**
  - 従来の一般グループ探索におけるアンカー判定（`src/discovery.cpp: identity_matches`）は明示的な cold ID（`model.cold_identities`）のみを照合していたため、ファームウェア不要で正規 runtime VID:PID のみを宣言する機種はアンカー候補として合致せず、`select_usb_group` コールバックに到達しなかった。
  - 本修正により、明示 cold ID 集合に加えて、登録済み `ModelEntry` が宣言する正規 runtime VID:PID（`model.model.vendor_id == candidate.vendor_id && model.model.product_id == candidate.product_id`）もアンカー候補として照合される。

### 2. 一般グループ探索と公開 projection への接続

- **selector による確定と未登録 ID の除外:**
  - アンカーとして照合された候補に対し、機種の `select_usb_group` コールバックが呼び出される。
  - コールバックが `DTV_USB_GROUP_COMPLETE` と判定した観測値（`CompleteGroupObservation`）のみが `collect_complete_groups` により抽出され、公開 projection（`dtv_list_enclosures` / `live_slots`）へ渡される。
  - 部分一致（`INCOMPLETE`）、不一致（`NO_MATCH`）、曖昧一致（`AMBIGUOUS`）は除外され、出力には含まれない。
  - 登録済み `ModelEntry` に存在しない未登録の VID:PID を持つデバイスは、アンカー判定を通過せず、機種として追加されることなく確実に除外される（selector にも到達しない）。

### 3. boot 専用経路との厳密な分離

- **明示 cold ID 限定の維持:**
  - ファームウェア送信のための boot 専用経路（`src/boot.cpp` の `cold_identity_matches`、`boot_selected_candidate`、`open_boot_group`）は、**明示的な cold ID（`model.cold_identities`）限定のまま変更なく維持される**。
  - したがって、runtime 宣言のみ（`cold_identity_count == 0`）のグループが誤って `open_boot_group` に渡された場合でも、`DTV_INTERNAL_RESULT_UNSUPPORTED` で安全に拒否され、USB provider が open されることはない。

### 4. 検証状況と境界

- **単体契約テスト検証:**
  - 既存内部 fixture（`tests/i2c1-partial-boot.cpp` / CTest: `i2c1_partial_boot_contract`）により、runtime-only（`cold_identity_count = 0`）の入力において、修正前の `selector_calls = 0`, `group = 0` から、修正後の `selector_calls = 1`, `DTV_USB_GROUP_COMPLETE group = 1` への変化を確認済みである。
  - 同じ runtime-only group を `open_boot_group` へ渡した場合の `DTV_INTERNAL_RESULT_UNSUPPORTED` 拒否、および未登録 ID（`0x5678:0x0002`）が group なし・`selector_calls = 0` で除外されることを確認済みである。
- **未実施境界と位置づけ:**
  - 公開 C API（`dtv_list_enclosures`）を直接呼び出す統合試験、製品実体（ASICEN 等）との結合、および USB 実機での動的確認は本 PR では未実施である。
  - 本限定修正をもって「特定機種の完成」や「SPI 安定版」とは呼ばない。
