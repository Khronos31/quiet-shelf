<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# 製品への組み込み

`tests/fixture` 配下の fixture は、`add_subdirectory` で Core を追加し、`MODELS` から製品ライブラリとモデル登録を生成して、Core のテンプレートからコマンドをビルドする最小構成の製品ディレクトリである。Core リポジトリのルートからは、次のコマンドを実行する。

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

fixture を独立した製品ディレクトリとしてビルドする場合:

```sh
cmake -S tests/fixture -B build/fixture -DBUILD_TESTING=ON
cmake --build build/fixture
ctest --test-dir build/fixture --output-on-failure
```

fixture の `CMakeLists.txt` は、製品名、コマンド名、バージョン、機種ソース名を指定して `dtv_add_product` を呼び出す。`MODELS` の各識別子は `lib/<識別子>.cpp` と名前付き `ModelEntry` aggregate に対応し、Core の CMake が静的アーカイブ用の機種表と C ABI アクセサを生成する。モデルを `SOURCES` に重複記載する必要はない。生成registryは同じ不変 `ModelEntry` へのconst pointerを保持し、FW bindとモデル列挙で別のmetadata表を作らない。FW entryが0件なら機種は未bindで、既定操作表へのfallbackはない。登録されていないコールバックはnullとなり、未実装として扱われる。CLIやオプションの規約はCore側に保持されるため、製品側でオプション対応表を複製する必要はない。`DEFAULT_MODEL` はビルド時に埋め込まれる。fixtureではこれを空にしており、暗黙にデバイスが選択されることはない。このメタデータのみを扱う初期CLIでは、動作時のデフォルト値はまだ適用されない。

各製品リポジトリでも、固定した Core のチェックアウトを追加したうえで、同じ関数を使用する。

```cmake
cmake_minimum_required(VERSION 3.20)
project(siano_userland LANGUAGES C CXX)

add_subdirectory(external/dtv11-core core)
dtv_add_product(NAME siano
                COMMAND sianoctl
                VERSION 0.1.0
                DEFAULT_MODEL ""
                MODELS px_s1ud)
```

各 `MODELS` 項目に対応するソースは、名前付きaggregateを定義する。VID/PID は確認済みの値を指定し、未確認値を登録してはならない。

```cpp
namespace dtv::spi {

extern const ModelEntry fixture_one_model{
    .model = {"Fixture One", 0x1234, 0x0001},
    .firmware = nullptr,
    .firmware_count = 0,
};

}
```

各製品archiveは一意な製品登録symbolを持つ。consumerは使う製品を `dtv_add_product_aggregate(target PRODUCTS ...)` で明示し、そのaggregateが製品・所属model一覧とC型model accessorを提供する。複数製品を同じconsumerへlinkするときも同じaggregateへ列挙する。既存の `dtv_add_product` と `ModelEntry` initializerは変更不要である。fixtureのaggregate例は `tests/fixture/CMakeLists.txt` を参照すること。

`include/dtv/cli.h` は、生成された main や非 C++ クライアントが使用する L3 C ABI である。`include/dtv/spi/types.h` は製品 SPI の C 互換値型を定義し、`include/dtv/spi/device_ops.hpp` はそれを使う C++ 製品向け SPI aggregate を定義する。文字列やコールバックテーブルは静的記憶域から借用され、プロセスの生存期間中は常に有効である。C API に渡されるバッファの所有権は呼び出し側にあり、API が呼び出し側所有のメモリを保持することはない。また、C ABI の規約に C++ 例外は含まれない。

開発中、製品リポジトリはこのリポジトリをレビュー済みコミット SHA の submodule として固定できる。ローカル環境で `CORE_SSH_URL` に private な Core の remote を設定し、`CORE_SHA` にレビュー済みコミット SHA を指定する。

```sh
git submodule add "$CORE_SSH_URL" external/dtv11-core
git -C external/dtv11-core checkout "$CORE_SHA"
git add external/dtv11-core
```

製品のビルドでは、この固定チェックアウトをサブディレクトリとして追加した上で `dtv_add_product` を呼び出す。後日の公開 quiet-shelf CI では、固定した Core のファイルを通常ファイルとして公開リポジトリへコピーする。この private submodule の URL は使用しないこと。`.github/workflows/ci.yml` にあるワークフローは、それらのファイルと共に配置された際、Linux および Windows/MSVC 上で同一の fixture ビルドと外部契約マトリクスを実行する。

この初期 SPI は、モデルの識別情報、VID:PID、C 互換境界型、および null 許容の操作スロットを持つ。完全なハードウェア API を定義しているわけではない。今後の各操作は末尾へ追加可能であるが、null コールバックが機能すると偽ってはならない。

## 操作要件と操作 ID（OperationRequirement）

受信機や機能の動作に必要な USB ファンクションの要件を定義する `OperationRequirement.operation`（`include/dtv/spi/device_ops.hpp`）および要件判定関数 `check_operation_requirements` に指定する操作 ID には、`include/dtv/spi/types.h` で定義された C 互換定数 `DTV_SPI_OPERATION_*`（`enum dtv_spi_operation`）を使用する。

これらの定数は、公開 L3 正本（`l3/dtv.h`）の `DTV_OP_*` と値および意味が完全に一致している。

| SPI 定数 | 値 | 公開正本定数 | 意味 |
|---|---:|---|---|
| `DTV_SPI_OPERATION_LNB_POWER` | 1 | `DTV_OP_LNB_POWER` | LNB 給電 |
| `DTV_SPI_OPERATION_CARD` | 2 | `DTV_OP_CARD` | カード操作 |
| `DTV_SPI_OPERATION_OUTPUT_PID_FILTER` | 3 | `DTV_OP_OUTPUT_PID_FILTER` | 出力 PID フィルター |
| `DTV_SPI_OPERATION_TUNE_ISDB_T` | 4 | `DTV_OP_TUNE_ISDB_T` | 地上波 ISDB-T 選局 |
| `DTV_SPI_OPERATION_TUNE_ISDB_S` | 5 | `DTV_OP_TUNE_ISDB_S` | 衛星 ISDB-S 選局 |
| `DTV_SPI_OPERATION_SELECT_SLOT` | 6 | `DTV_OP_SELECT_SLOT` | slot 選択 |
| `DTV_SPI_OPERATION_SELECT_TSID` | 7 | `DTV_OP_SELECT_TSID` | TSID 選択 |
| `DTV_SPI_OPERATION_SIGNAL` | 8 | `DTV_OP_SIGNAL` | signal 取得（lock 測定を含む） |
| `DTV_SPI_OPERATION_LNA` | 9 | `DTV_OP_LNA` | LNA 設定 |

独立した LOCK 操作 ID は存在せず、lock 測定は signal 取得（`DTV_SPI_OPERATION_SIGNAL`）に含まれる。

### 最小の使い方

製品側で受信機の操作要件テーブルを定義する際は、以下のように `DTV_SPI_OPERATION_*` を指定する。

```cpp
const dtv::spi::OperationRequirement requirements[]{
    {receiver, DTV_SPI_OPERATION_TUNE_ISDB_T, required_functions, function_count},
};
```

`OperationRequirement.operation` フィールドは既存の `uint32_t`（C++ では `std::uint32_t`）のままであり、フィールド幅・配置・構造体レイアウト（ABI）に変更はない。本定義は操作 ID の値識別子と対応契約を規定したものであり、動的操作処理の追加や未接続の L3/TS 処理を伴うものではない。

## ファームウェアパッケージの期待値検証（FunctionEntry）

`include/dtv/spi/device_ops.hpp` の `FunctionEntry` 末尾には、ファームウェアパッケージ全体の書き込み前検証（preflight）を行うための任意フィールドが用意されている。

- `expected_package_size`（`std::uint64_t`）: パッケージの期待全長。`0` は未指定を意味し、非ゼロが指定された場合は入力パッケージ全長との完全一致を要求する。
- `expected_package_sha256`（`const char *`）: パッケージ全体の期待 SHA-256 16進文字列。`nullptr` は未指定を意味し、指定された場合はパッケージ全体の SHA-256 ハッシュを検証する。

### 登録責務と検証の動作

本フィールドは全製品一律で必須とされるものではなく、機種の確定仕様に基づいて製品側がマニフェストに登録する。Core が製品固有のハッシュ値やサイズを固定したり、全機種へ強制することはない。例えば、torne（CECH-ZD1J）のように確定仕様で全長（3428 bytes）および全体 SHA-256 の双方の検証を要求する機種では、製品側がマニフェストにそれらの値を指定することで、書き込み前の共通検証を利用する。

各イメージスライスごとの `offset`、`size`、`sha256` 検証も維持され、全体検証とイメージ検証は独立して行われる。全長・全体ハッシュが未指定の場合は、従来どおり各イメージスライスのみが検証され、既存製品におけるファイルや offset の扱いはそのまま維持される。

### 複数マニフェストとエラー処理

- 同一の物理デバイス（candidate）に属する複数のファンクションマニフェストで、異なる非ゼロ全長または異なる全体ハッシュが指定された場合は、最初のファームウェア書き込み前に `INVALID_ARGUMENT` で拒否される。
- 未指定（`0` または `nullptr`）のマニフェストが存在しても、同一デバイス内の他のマニフェストが指定した期待値を打ち消すことはない。
- 期待値の照合は candidate 単位で処理されるため、別の物理デバイスに対するパッケージ入力が先行して検証されることはない。
- 本検証はデバイス書き込み前のパッケージ preflight 経路で行われ、レガシーな `BootRequest`（claim 前の bundle 経路）とは区別される。

### 最小の使い方（抜粋）

```cpp
extern const dtv::spi::FunctionEntry sample_functions[]{
    {
        .role = 0,
        .stages = sample_stages,
        .stage_count = 1,
        .create_state = nullptr,
        .destroy_state = nullptr,
        .expected_package_size = 3428,
        .expected_package_sha256 = "...",
    },
};
```

## 内部結果と診断メッセージ（dtv_internal_result）

製品コールバックや内部処理が返す `dtv_internal_result`（`include/dtv/spi/types.h`）は、既存の `code`（`int32_t`）および `detail`（`uint32_t`）のフィールド幅・順序・意味を維持したまま、末尾に 128 バイトのインライン診断配列（`char diagnostic[DTV_INTERNAL_RESULT_DIAGNOSTIC_CAPACITY]`、定数 `DTV_INTERNAL_RESULT_DIAGNOSTIC_CAPACITY = 128u`）を追加している。

診断内容は `dtv_internal_result` の値自身が所有するため、コールバック復帰後も値コピーによってその内容と寿命が独立して保持される。スタック上のポインタ、スレッドローカルストレージ（TLS）、共有ミュータブル変数、動的メモリ確保（アロケータ）に依存しない。なお、`diagnostic` に格納される文字列は診断参考情報であり、呼び出し側がエラーコード（`code`）に基づいて行う条件分岐を置き換えるものではない。

### 初期化規則とヘルパー関数

部分代入による未初期化領域の残留を防ぐため、結果オブジェクトは必ず構造体全体をゼロ初期化してから生成する規則とする。C/C++ 共通のインラインヘルパー関数が提供されている。

- `dtv_internal_result dtv_internal_result_make(int32_t code, uint32_t detail)`:
  構造体全体をゼロ初期化（`{0}`）した上で `code` と `detail` を設定した結果値を返す。
- `void dtv_internal_result_set_diagnostic(dtv_internal_result *result, const char *diagnostic, size_t length)`:
  指定された `length` バイトまでを上限付きでコピーし、最大 127 バイトをコピーして末尾を必ず NUL 終端する。元の `code` および `detail` は保持される。
- `size_t dtv_internal_result_diagnostic_length(const dtv_internal_result *result)`:
  配列範囲内（最大 128 バイト）でのみ文字数を読み取る。NUL 終端が存在しない場合も配列外アクセスを行わず、安全に最大 128 を返す。

#### 最小の使い方

```c
dtv_internal_result result = dtv_internal_result_make(DTV_INTERNAL_RESULT_USB_IO, 0);
dtv_internal_result_set_diagnostic(&result, "bulk transfer timeout", 21);
return result;
```

### ABI 変更と再ビルド

`dtv_internal_result` の値返しサイズが増加したため、内部 ABI が変更されている。旧 SPI で構築された製品アーカイブと新 Core を混在させることはできず、全製品コールバックおよび製品アーカイブを同一の SPI に対して再ビルドする必要がある。

### 公開境界と未決事項

本内部 SPI の診断欄は内部での生成・保持・値伝播を対象としたものであり、公開 L3 の `dtv_error.message` アダプターへの接続は未接続である。

成功時や診断なし時の公開バッファ消去規則、文字エンコーディングおよびマルチバイト文字境界での切詰め処理、ならびに主たる失敗（primary failure）とクリーンアップ失敗（cleanup failure）の個別報告仕様は未決事項として扱われる。本内部 SPI の定義によってこれらに関する公開保証を新たに追加するものではなく、既存のクリーンアップ失敗破棄の動作（主たる失敗がクリーンアップ結果で上書きされない挙動）もそのまま維持される。

## 内部 USB 転送所有と同期 I/O シム（dtv_usb_io）

製品コールバックが使用する `dtv_usb_io`（`include/dtv/spi/types.h`）の同期関数シグネチャ、戻り値（`dtv_internal_result`）、および転送長（actual length）の意味は維持されている。Core 内部では、直接の同期 libusb 呼び出しに代わり、非同期 libusb transfer と呼び出し元スレッド上の同期待機（waiter）を組み合わせた内部シム経由で処理される。

### 内部イベントサービスとデータ転送

- **イベントサービスとレジストリ**: `UsbContext::State` ごとに 1 つのイベントサービススレッドが起動され、transfer request registry を所有・管理する。
- **バッファのステージングとコピー**: OUT 転送および control 転送の送信データは submit 前にリクエスト専用バッファへコピーされる。IN 転送は Core 内部のステージングバッファで受信され、リクエスト完了後に呼び出し側のバッファへコピーバックされる。
- **スレッド分離**: libusb の completion コールバックは転送状態と受信長を記録して waiter を起床させる処理のみを行い、製品コールバックをイベントスレッドから呼び出すことはない。waiter は completion コールバックがイベントハンドラから復帰したことを確認した上で結果を読み出す。

### RAII 所有権とリースのライフサイクル

各リクエストは完了してレジストリから外れるまで handle lease を保持する。この lease は context、デバイスハンドル、インターフェース、および Core が複製・所有する FD（owned FD lease）の有効性を保護する。すべてのリクエストが完了して最後の lease が解放された時点で、インターフェースが逆順に解放（reverse release）され、ハンドルおよび FD がクローズされる。通常オープンと `open_usb_fd` は同一の lease / runtime 型を使用する。

### タイムアウトの扱い

内部 USB I/O における `timeout = 0` は即時返却（no-wait）を、`UINT64_MAX` は無期限待機を意味する。これは公開 L3 API における `timeout_ms = 0`（無期限）という契約とは区別される。

### テスト治具と未接続事項

内部 fake transfer backend は、実機に依存せず Core の I/O バインディング、lease、FD 所有状態を検証するためのテスト用 seam である。通常環境での `libusb_open` や claim、`libusb_wrap_sys_device` の実行、実機 USB での転送、ならびに Android や Chrome（WebUSB）環境での動作を証明するものではない。

また、後続段階で扱われる cancel / close ライフサイクル、公開 PENDING / CLOSED 状態、ファームウェア再列挙、FD / token 供給、TS 処理、attachment latch は未接続であり、本実装をもって B2 全体が完了したわけではない。

## ライセンスと由来

本リポジトリの Core ソースコードは dtv11 向けに新規作成されたものであり、読み取り専用の参照用 Core からコードをコピーしていない。各ソースファイルおよび文書ファイルには GPL-2.0-or-later の SPDX 識別子が付与されている。将来サードパーティ製ソースを追加する場合は、取り込み前にライセンスの互換性を確認しなければならない。

## 出力エラーと SIGPIPE

C API は stdout への書き込みと flush を確認し、失敗した場合は 70 を返す。プロセスのシグナル設定（disposition）は変更しない。POSIX 上の生成コマンドは、`main` で SIGPIPE を無視（SIG_IGN）に設定するため、パイプが閉じられた場合も通常の書き込みエラーとして扱い 70 を返す。インプロセスで呼び出す側は自身の SIGPIPE ポリシーを維持する。デフォルトのポリシーのままでは、C API がリターンする前に、閉じた stdout パイプによってホストプロセスが終了する可能性がある。

## 引数判定の段階

CLI は元の引数配列を Core の C API へ渡す。Core は通常の引数検証に先立ってメタデータを処理する。help は version より優先され、両者とも models や無効な引数より優先される。models は単独で指定された場合のみ受け付けられる。PR147 L1 契約では、未知の入力に対するエラーコードとして 2 を割り当てている。サポートされていない既知のハードウェア機能は、後日実装される別の結果（コード 3）となる。本実装段階では既知のコマンドとしてのハードウェア操作を実装していないため、メタデータ以外の引数は未知として扱われ、コード 2 で拒否される。コマンドが追加された後は、Core がまずそのコマンドを認識し、選択されたデバイスのコールバックテーブルを用いてハードウェアがそれをサポートしているかを判定する。デーモンのファームウェアなど実行時に必要となる入力は、メタデータのみの要求には適用されず、ここでは実装されていない。
