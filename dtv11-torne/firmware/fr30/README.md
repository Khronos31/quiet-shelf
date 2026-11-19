<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# FR30 クロス開発ツールと入力情報

CECH-ZD1J の内蔵ブリッジチップ（MB86H47A）に搭載された FR30 コア向けファームウェアの生成ツールおよびビルド入力に関する技術情報である。

ファームウェア全体の構成、配布用バイナリ諸元、ライセンス、アセットの役割等については [ファームウェア概要（../README.md）](../README.md) を参照のこと。

## 1. クロスアセンブラ（GNU binutils）の固定入力情報

ファームウェアの各コア画像生成スクリプト（`cmake/fr30_widen.cmake`, `cmake/fr30_core0.cmake`, `cmake/fr30_core1.cmake`）は、GNU binutils 2.47 の `fr30-elf` ツールチェーン（`as`, `ld`, `objcopy`）を必要とする。製品の通常ビルドではコミット済みバイナリアセットを使用するため、binutils は製品のランタイム依存ではない。

参考実装で記録された固定入力値および検証情報は以下のとおりである。

| 項目 | 値 |
|---|---|
| 対象ツール | GNU binutils `2.47`、target `fr30-elf` |
| ソースアーカイブ URL | `https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz` |
| `.tar.xz` SHA-256 | `154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff` |
| `.sig` SHA-256 | `9fa42a8770ff1fdb3059a6d5c7b7030899acee58b343364bc42b6facfa35ce65` |
| 署名者 fingerprint | `3A24BC1E8FB409FA9F14371813FCEF89DD9E3C4F`（Nick Clifton） |
| 参考: ローカル検証時の keyring SHA-256 | `8013de59880f7a59f53b6bd207f4d2a408d67b84a82fa9cba1dbbf443428e2db` |

### 入力アーカイブの検証条件
- `gpgv` により配布署名（`.sig`）を検証し、`Good signature` であること、および署名者の fingerprint（上記）を確認する。
- ソースアーカイブ（`.tar.xz`）の SHA-256 ハッシュ値が上記固定値と一致することを確認する。
- ※ GNU の keyring は随時更新されるため、配布物に固定同梱はしていない。上記テーブルに記載された keyring の SHA-256 は、**過去のローカル検証環境で用いられた keyring の記録**であり、本リポジトリの配布固定値ではなく、また現在の作業環境で署名確認済みであることを示すものではない。

## 2. 過去の検証時における binutils ビルド条件（参考記録）

参考実装において binutils 2.47（`fr30-elf`）をローカル構築した際の条件は以下のとおり記録されている。これらは特定の環境における過去の検証記録であり、記載されたホスト環境パスや特定のコンパイラバージョン（gcc-14 等）は、本リポジトリの全ビルド環境に対する必須要件・前提条件ではない。

### configure オプション
binutils のビルドディレクトリから以下のように実行する。

```sh
../binutils-2.47/configure --target=fr30-elf --disable-nls \
  --disable-werror --disable-gdb --disable-gdbserver --disable-sim \
  --disable-gprofng
```

### 過去の検証環境設定（参考）
- `PATH`: `/usr/bin:/bin`（共有ラッパー `/config/.tools/bin/gcc` 等を経由しない）
- `CC` / `CXX`: `/usr/bin/gcc-14` / `/usr/bin/g++-14`（ビルド用コンパイラ `CC_FOR_BUILD`, `CXX_FOR_BUILD`, `BUILD_CC`, `BUILD_CXX` も同様の実体を使用）
- configure キャッシュ指定: `acx_cv_cc_gcc_supports_ada=no`
- ビルド実行: `nice -n 10 make -j2 all-binutils all-gas all-ld`

## 3. CMake スクリプトとの対応

FR30 コア画像の生成用 CMake スクリプト（`cmake/fr30_*.cmake`）実行時に binutils のパスが未指定の場合に本ファイルを案内する。実際の生成手順やアセット組立の流れについては、[ファームウェア概要（../README.md）](../README.md) の第 4 節を参照のこと。
