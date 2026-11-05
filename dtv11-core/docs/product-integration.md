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

機種表、`product_models()`、および生成 main から呼ばれる C 型モデルアクセサは Core の CMake が生成し、製品側に定型登録コードは不要である。fixture の実例は `tests/fixture/lib/fixture_one.cpp` と `fixture_two.cpp` を参照すること。

`include/dtv/cli.h` は、生成された main や非 C++ クライアントが使用する L3 C ABI である。`include/dtv/spi/types.h` は製品 SPI の C 互換値型を定義し、`include/dtv/spi/device_ops.hpp` はそれを使う C++ 製品向け SPI aggregate を定義する。文字列やコールバックテーブルは静的記憶域から借用され、プロセスの生存期間中は常に有効である。C API に渡されるバッファの所有権は呼び出し側にあり、API が呼び出し側所有のメモリを保持することはない。また、C ABI の規約に C++ 例外は含まれない。

開発中、製品リポジトリはこのリポジトリをレビュー済みコミット SHA の submodule として固定できる。ローカル環境で `CORE_SSH_URL` に private な Core の remote を設定し、`CORE_SHA` にレビュー済みコミット SHA を指定する。

```sh
git submodule add "$CORE_SSH_URL" external/dtv11-core
git -C external/dtv11-core checkout "$CORE_SHA"
git add external/dtv11-core
```

製品のビルドでは、この固定チェックアウトをサブディレクトリとして追加した上で `dtv_add_product` を呼び出す。後日の公開 quiet-shelf CI では、固定した Core のファイルを通常ファイルとして公開リポジトリへコピーする。この private submodule の URL は使用しないこと。`.github/workflows/ci.yml` にあるワークフローは、それらのファイルと共に配置された際、Linux および Windows/MSVC 上で同一の fixture ビルドと外部契約マトリクスを実行する。

この初期 SPI は、モデルの識別情報、VID:PID、C 互換境界型、および null 許容の操作スロットを持つ。完全なハードウェア API を定義しているわけではない。今後の各操作は末尾へ追加可能であるが、null コールバックが機能すると偽ってはならない。

## ライセンスと由来

本リポジトリの Core ソースコードは dtv11 向けに新規作成されたものであり、読み取り専用の参照用 Core からコードをコピーしていない。各ソースファイルおよび文書ファイルには GPL-2.0-or-later の SPDX 識別子が付与されている。将来サードパーティ製ソースを追加する場合は、取り込み前にライセンスの互換性を確認しなければならない。

## 出力エラーと SIGPIPE

C API は stdout への書き込みと flush を確認し、失敗した場合は 70 を返す。プロセスのシグナル設定（disposition）は変更しない。POSIX 上の生成コマンドは、`main` で SIGPIPE を無視（SIG_IGN）に設定するため、パイプが閉じられた場合も通常の書き込みエラーとして扱い 70 を返す。インプロセスで呼び出す側は自身の SIGPIPE ポリシーを維持する。デフォルトのポリシーのままでは、C API がリターンする前に、閉じた stdout パイプによってホストプロセスが終了する可能性がある。

## 引数判定の段階

CLI は元の引数配列を Core の C API へ渡す。Core は通常の引数検証に先立ってメタデータを処理する。help は version より優先され、両者とも models や無効な引数より優先される。models は単独で指定された場合のみ受け付けられる。PR147 L1 契約では、未知の入力に対するエラーコードとして 2 を割り当てている。サポートされていない既知のハードウェア機能は、後日実装される別の結果（コード 3）となる。本実装段階では既知のコマンドとしてのハードウェア操作を実装していないため、メタデータ以外の引数は未知として扱われ、コード 2 で拒否される。コマンドが追加された後は、Core がまずそのコマンドを認識し、選択されたデバイスのコールバックテーブルを用いてハードウェアがそれをサポートしているかを判定する。デーモンのファームウェアなど実行時に必要となる入力は、メタデータのみの要求には適用されず、ここでは実装されていない。
