<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# 製品への組み込み

`tests/fixture` 配下の fixture は、`add_subdirectory` で Core を追加し、`lib/*.cpp` から製品ライブラリを宣言して、Core のテンプレートからコマンドをビルドする最小構成の製品ディレクトリである。Core リポジトリのルートからは、次のコマンドを実行する。

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

fixture の `CMakeLists.txt` は、製品名、コマンド名、バージョンを指定して `dtv_add_product` を呼び出す。`lib/devices.cpp` には、生成コマンドが使用する `DeviceOps` テーブルとモデルのアクセサを記述する。生成された Core の main 関数が C ABI シンボルを呼び出すため、アクセサは製品の静的アーカイブから解決される。登録されていないコールバックは null となり、未実装として扱われる。CLI やオプションの規約は Core 側に保持されるため、製品側でオプション対応表を複製する必要はない。`DEFAULT_MODEL` はビルド時に埋め込まれる。fixture ではこれを空にしており、暗黙にデバイスが選択されることはない。このメタデータのみを扱う初期 CLI では、動作時のデフォルト値はまだ適用されない。

各製品リポジトリでも、固定した Core のチェックアウトを追加したうえで、同じ関数を使用する。

```cmake
cmake_minimum_required(VERSION 3.20)
project(siano_userland LANGUAGES C CXX)

add_subdirectory(external/dtv11-core core)
dtv_add_product(NAME siano
                COMMAND sianoctl
                VERSION 0.1.0
                DEFAULT_MODEL ""
                SOURCES lib/models.cpp)
```

fixture 内の最初のモデルテーブルは、次のような構成をとる。

```cpp
std::span<const DeviceOps> product_devices()
{
    static constexpr std::array<DeviceOps, 2> devices{{
        {{"Fixture One", 0x1234, 0x0001}, nullptr, nullptr},
        {{"Fixture Two", 0x5678, 0x9abc}, nullptr, nullptr},
    }};
    return devices;
}
```

このテーブルは `libfoo.a` 内のソースファイルに配置する。製品側では、生成された main から呼ばれる C 型のモデルアクセサ2つも実装する。完全に動作するファイル全体は `tests/fixture/lib/devices.cpp` を参照すること。この fixture にはハードウェア制御パスが存在しないため、`DeviceOps` の open/close スロットには初期値として null を指定している。

`include/dtv/cli.h` は、生成された main や非 C++ クライアントが使用する C ABI である。C 互換の型のみを公開する。`include/dtv/spi/device_ops.hpp` は、モデル記述子と null 許容の操作コールバックを定義する C++ 製品向け SPI である。文字列やコールバックテーブルは静的記憶域から借用され、プロセスの生存期間中は常に有効である。C API に渡されるバッファの所有権は呼び出し側にあり、API が呼び出し側所有のメモリを保持することはない。また、C ABI の規約に C++ 例外は含まれない。

開発中、製品リポジトリはこのリポジトリをレビュー済みコミット SHA の submodule として固定できる。ローカル環境で `CORE_SSH_URL` に private な Core の remote を設定し、`CORE_SHA` にレビュー済みコミット SHA を指定する。

```sh
git submodule add "$CORE_SSH_URL" external/dtv11-core
git -C external/dtv11-core checkout "$CORE_SHA"
git add external/dtv11-core
```

製品のビルドでは、この固定チェックアウトをサブディレクトリとして追加した上で `dtv_add_product` を呼び出す。後日の公開 quiet-shelf CI では、固定した Core のファイルを通常ファイルとして公開リポジトリへコピーする。この private submodule の URL は使用しないこと。`.github/workflows/ci.yml` にあるワークフローは、それらのファイルと共に配置された際、Linux および Windows/MSVC 上で同一の fixture ビルドと外部契約マトリクスを実行する。

この初期 SPI は、モデルの識別情報、VID:PID、および null 許容の open/close スロットのみを持つ。完全なハードウェア API を定義しているわけではない。今後の各操作は、Core 側の動作が実装されるにつれて SPI に追加可能であるが、null コールバックが機能すると偽ってはならない。

## ライセンスと由来

本リポジトリの Core ソースコードは dtv11 向けに新規作成されたものであり、読み取り専用の参照用 Core からコードをコピーしていない。各ソースファイルおよび文書ファイルには GPL-2.0-or-later の SPDX 識別子が付与されている。将来サードパーティ製ソースを追加する場合は、取り込み前にライセンスの互換性を確認しなければならない。

## 出力エラーと SIGPIPE

C API は stdout への書き込みと flush を確認し、失敗した場合は 70 を返す。プロセスのシグナル設定（disposition）は変更しない。POSIX 上の生成コマンドは、`main` で SIGPIPE を無視（SIG_IGN）に設定するため、パイプが閉じられた場合も通常の書き込みエラーとして扱い 70 を返す。インプロセスで呼び出す側は自身の SIGPIPE ポリシーを維持する。デフォルトのポリシーのままでは、C API がリターンする前に、閉じた stdout パイプによってホストプロセスが終了する可能性がある。

## 引数判定の段階

CLI は元の引数配列を Core の C API へ渡す。Core は通常の引数検証に先立ってメタデータを処理する。help は version より優先され、両者とも models や無効な引数より優先される。models は単独で指定された場合のみ受け付けられる。PR147 L1 契約では、未知の入力に対するエラーコードとして 2 を割り当てている。サポートされていない既知のハードウェア機能は、後日実装される別の結果（コード 3）となる。本実装段階では既知のコマンドとしてのハードウェア操作を実装していないため、メタデータ以外の引数は未知として扱われ、コード 2 で拒否される。コマンドが追加された後は、Core がまずそのコマンドを認識し、選択されたデバイスのコールバックテーブルを用いてハードウェアがそれをサポートしているかを判定する。デーモンのファームウェアなど実行時に必要となる入力は、メタデータのみの要求には適用されず、ここでは実装されていない。
