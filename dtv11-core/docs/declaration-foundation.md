<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# 宣言基盤・C ABI・CMake登録の使い方と到達範囲

本文書は、PR21でCoreリポジトリに導入された宣言基盤、C ABI、およびCMake製品登録（`dtv_product`）の仕様と利用方法を、後続のCoreおよび製品実装者向けに説明するものである。

## 3層構成と生成物の区分

本アーキテクチャでは、呼び出し経路と責務を以下の3層に明確に分離している。

1. **Core共通層（`dtv_*`）:** 共通の公開C ABI（`include/dtv/dtv.h`）。内部ディスパッチテーブルを持ち、登録された製品・機種の操作を呼び出す。
2. **製品層（`dtv_*_<product>`）:** CMakeが生成するC ABIラッパーおよび登録エントリ。手書き製品コードとCoreを仲介する。
3. **機種層（`dtv_*_<product>_<model>` / `dtv_model_impl_*`）:** 機種ごとの手書き実装関数。機能宣言に応じた処理を実装する。

ビルド時に生成されるレジストリ（`${PRODUCT_NAME}_registry.cpp`）、CLI main（`${PRODUCT_COMMAND}_main.cpp`）、アセットヘッダ（`${PRODUCT_NAME}_firmware.hpp`）、および未宣言操作のstubは、上記の手書きコードとは区別されるビルド生成物である。

なお、現時点では**公開opaque handle（`dtv_device`等）の生成や、handleから製品・機種への実解決、実機操作ランタイムは未接続・未実装**である。公開ヘッダに宣言が存在しても、全シンボルが実装・解決済みではない点に注意すること。

## 機能宣言と型検査（C++20 SPI）

製品および機種の機能は、`include/dtv/spi/feature.hpp` のマクロを用いて宣言する。

- **機能一覧（`dtv::spi::Feature`）:** `LnbPower`, `Card`, `OutputPidFilter`, `TuneIsdbT`, `TuneIsdbS`, `SelectSlot`, `SelectTsid`, `Signal`, `Lna`, `Firmware` の10種。
- **製品機能宣言（`DTV_PRODUCT`）:** 製品がサポートしうる機能集合を `DTV_FEATURES(...)` で定義する。
- **機種機能宣言（`DTV_MODEL`）:** 機種がサポートする機能集合、ファームウェア宣言、基本情報を定義する。機種機能は製品機能の部分集合（subset）である必要があり、違反時は `static_assert` によりコンパイルエラーとなる。
- **ファームウェア宣言:** `DTV_NO_FIRMWARE` または `DTV_FIRMWARE(asset_name)` のいずれかの指定が必須であり、未指定時は `static_assert` で検出される。
- **モデル定義（`DTV_DEFINE_MODEL`）:** 機種の公開操作テーブルと内部レコードを生成する。

```cpp
// tests/fixture/lib/fixture_one.cpp より引用
DTV_PRODUCT(fixture, DTV_FEATURES(::dtv::spi::Feature::LnbPower));
DTV_MODEL(fixture, fixture_one, DTV_FEATURES(::dtv::spi::Feature::LnbPower), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"Fixture One", 0x1234, 0x0001}));
DTV_DEFINE_MODEL(fixture, fixture_one);
```

## 7操作行集合・未宣言stub・強参照リンクエラー

正本に対応する公開モデル操作は、`DTV_PUBLIC_MODEL_OPS` で定義される以下の7操作である。

1. `lnb_on`（要求機能: `LnbPower`）
2. `lnb_off`（要求機能: `LnbPower`）
3. `tune_by_frequency`（要求機能: `TuneIsdbT` または `TuneIsdbS` のいずれか）
4. `get_signal`（要求機能: `Signal`）
5. `set_pid_filter`（要求機能: `OutputPidFilter`）
6. `send_firmware`（要求機能: `Firmware`）
7. `send_default_firmware`（要求機能: `Firmware`）

### 未宣言stubと強参照
- **未宣言操作:** 機種機能に含まれない操作は、ラッパー内で未サポートと判定され `DTV_ERROR_UNSUPPORTED` を即座に返すstub動作となる。手書き実装関数は参照されない。
- **宣言済み操作:** 機能を宣言した操作は、手書き実装シンボル `dtv_model_impl_<op>_<product>_<model>` を強参照する。このため、機能を宣言しながら実装を定義し忘れた場合、リンク時に未定義シンボルエラー（undefined reference）として確実に検出される。

## ファームウェア資産と手書き例外

- **資産埋め込み:** CMakeで `FIRMWARE <name>=<path>` を指定すると、バイナリを埋め込んだ構造体 `dtv::spi::firmware_assets::<name>` を含むヘッダが生成される。
- **default wrapper:** 機種が `DTV_FIRMWARE(<name>)` を宣言している場合、生成される `send_default_firmware` ラッパーは埋め込みバイト列（`data`, `size`）を取り出して手書きの `dtv_model_impl_send_firmware_*` へ渡す。
- **no-FW:** `DTV_NO_FIRMWARE` を宣言した機種では、ファームウェア送信操作は常に `DTV_ERROR_OK` を返し、手書き実装の作成は不要である。
- **手書き製品_to_all例外:** 製品全体の全機器に対する一括送信関数 `dtv_send_default_firmware_to_all_<product>`（例: `dtv_send_default_firmware_to_all_fixture`）は自動生成されない。製品側の共通ソース（`SOURCES`）で必ず手書き実装を定義しなければならない。

```cpp
// tests/fixture/src/asset_model.cpp より引用
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
  - `${PRODUCT_NAME}_api.h`: 公開Cヘッダ（ロード関数、個別メタデータ関数）。
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
  - `dtv_get_product_support(product, op, &tribool, err)`: 製品全体でのサポート状況を `DTV_YES`, `DTV_NO`, `DTV_UNKNOWN`（機種によって異なる）のtriboolで返す。
  - `dtv_get_model_support(model, op, &tribool, err)`: 指定機種のサポート状況を `DTV_YES` または `DTV_NO` で返す。

## CLI接続・既存レガシーとの境界・未実装範囲

- **CLI接続:** 生成された main は、argc/argv を変更せずそのまま既存Coreの `dtv_cli_main` へ渡す。L1の全parserや全subcommandの実装が完了したわけではない。
- **レガシーとの境界:** 既存のメタデータAPI（`dtv_model_count`, `dtv_model_get`）や旧内部ランタイム（USB、discovery、firmware転送）は残存しているが、本PRで導入された新L3 registryとは分離されている。正本との識別子衝突を回避するため、内部レガシー型・状態名は機械的に改名されている。
- **未実装範囲:** USB I/O、実機へのfirmware転送と再列挙、TS処理、B-CAS/カード処理、および公開opaque handleの生成・解決ランタイムは未実装であり、後続の工程で接続される。
- **決定事項:** 正本における決定48（slot分類）および決定49（runtime slug）は決定済みであり、未決事項を独断で仕様変更しない。

## 検証状況

- **実施済み検証:**
  - WSL上のGNUツールチェーン環境におけるfresh build。
  - 対象5件のテスト通過: `l3_c_consumer`, `feature_declaration_contract`, `l3_foundation_contract`, `asset_firmware_contract`, `product_dispatch_contract`。
  - CLIバージョン取得プローブ: `fixturectl --version`（終了コード0、出力 `fixture 1.2.3`）。
  - 定義欠落リンクプローブ: 宣言済み機能の実装を欠落させた際のリンクエラー検出を確認。
- **未実施事項:** MSVC、wasm-ld、実機での検証、および独立QA・由来確認は後続工程とし、本PRのスコープ外である（spikeでの検証結果は今回のhead検証には転用しない）。
