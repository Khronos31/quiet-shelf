# FR30生成の土台

このディレクトリは、製品に将来同梱するFR30機械語を、固定したGNU binutilsと
自作ソースから再生成・検査するための土台を示す。**この増分にはWidenの
read/write handler候補の自作ソースと両bank画像がある。Core 0 / Core 1の
ファームウェア、ROMパッチの適用、共有RAM配置はない。** 通常のCMakeビルドと
配布物は、クロスアセンブラを要求しない。

## 固定入力

| 入力 | 固定値 |
| --- | --- |
| binutils | 2.47（`fr30-elf`ターゲット） |
| 配布元 | `https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz` |
| `.tar.xz` SHA-256 | `154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff` |
| `.sig` SHA-256 | `9fa42a8770ff1fdb3059a6d5c7b7030899acee58b343364bc42b6facfa35ce65` |
| 署名者 | `3A24BC1E8FB409FA9F14371813FCEF89DD9E3C4F`（Nick Clifton） |
| ローカル検証のkeyring SHA-256 | `8013de59880f7a59f53b6bd207f4d2a408d67b84a82fa9cba1dbbf443428e2db` |

keyringはGNUが更新するため配布物へ固定同梱しない。`gpgv`で`Good signature`を
確認したうえで、署名者のfingerprintが上の値であることを確かめる。ソースの
SHA-256は固定値と一致しなければならない。署名・ハッシュの検証はビルド環境の
入力検査であり、GNUツールを製品のruntime依存にはしない。

検証した実行条件は、`PATH=/usr/bin:/bin`、`CC=/usr/bin/gcc-14`、
`CXX=/usr/bin/g++-14`、`CC_FOR_BUILD`等も同じ実体、`acx_cv_cc_gcc_supports_ada=no`、
`nice -n 10`、`make -j2 all-binutils all-gas all-ld`。共有のコンパイラ
ラッパーは使わない。

```sh
mkdir -p binutils-build
cd binutils-build
env PATH=/usr/bin:/bin CC=/usr/bin/gcc-14 CXX=/usr/bin/g++-14 \
  CC_FOR_BUILD=/usr/bin/gcc-14 CXX_FOR_BUILD=/usr/bin/g++-14 \
  BUILD_CC=/usr/bin/gcc-14 BUILD_CXX=/usr/bin/g++-14 \
  acx_cv_cc_gcc_supports_ada=no nice -n 10 \
  ../binutils-2.47/configure --target=fr30-elf --disable-nls \
  --disable-werror --disable-gdb --disable-gdbserver --disable-sim \
  --disable-gprofng
nice -n 10 make -j2 all-binutils all-gas all-ld
```

## 生成と検査

通常のビルドは次のとおりで、`TORNE_FR30_BINUTILS_DIR`を指定しない。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

再生成する保守者は、binutilsのbuild tree（`gas/as-new`、`ld/ld-new`、
`binutils/objcopy`を含む）を指定して明示targetを実行する。指定がない、または
ツールが見つからない場合、このtargetは失敗する。

```sh
cmake -S . -B build-fr30 -G Ninja -DTORNE_FR30_BINUTILS_DIR=/path/to/binutils-build
cmake --build build-fr30 --target torne_fr30_generate
```

同じツールを指定してconfigureした場合だけ、`torne_fr30_generation`テストが
CTestへ登録される。targetとテストは同じ`cmake/fr30_generate.cmake`を使い、
2つの独立した出力ディレクトリへ生成してbyte一致を確認する。

## 検査対象のfixture

`tests/fixtures/fr30/probe.s`は今回書いた非実機用の小さな命令例で、機器I/Oを
持たない。`widen.ld`、`widen-bank1.ld`、`core0.ld`、`core1.ld`は配置先だけを
分けたfixture用リンカスクリプトである。これらは製品FWとしてリンク・埋込しない。

検査内容:

- `ldi:8`、`ldi:20`、`nop`、`ret`の既知encoding `c2a09b1123459fa097209fa0`
- ELF32、big endian、`EM_FR30`（84）、`ET_EXEC`
- Widen両バンク（`0xE1000` / `0xE1800`）とCore 0/1（`0xA0000`）の個別entry
- `.bin`が4 B転送単位に整列していること
- 別出力ディレクトリでの`.bin`・`.elf`再生成一致
- 容量上限超過、entry欠落、容量上限未指定をリンク時に検出すること

fixtureの容量上限は検査用の値である。Widenの`0x800` Bは固定coax候補であり、
Core 0/1のローカルRAM容量は契約で未確定なので、製品の確定値として扱わない。
リンカスクリプトは`__fr30_text_limit`の明示指定を要求し、未指定のまま生成を
通さない。

## Widen候補（read/write handler）

`fr30/src/widen.s` は、CECH-ZD1J主CPUのEP0メモリ要求ハンドラへ入れる
read/write handlerの自作候補である。`fr30/ld/widen-bank0.ld` と
`widen-bank1.ld` が、TWDNヘッダを含む画像をバンク0 `0xE1000`、バンク1
`0xE1800`へ配置する。`fr30/generated/` に追跡する生成物を置く。

| ファイル | 内容 |
| --- | --- |
| `src/widen.s` | 自作FR30 assembly。R4の24bitアドレス、R8/R10の要求状態を使う |
| `ld/widen-bank0.ld`、`ld/widen-bank1.ld` | ヘッダ`+0x00`〜`+0x1F`、コード`+0x20`以降、entry・容量・4B整列のASSERT |
| `generated/widen-bank0.elf`、`.bin` | バンク0のELFとTWDN画像 |
| `generated/widen-bank1.elf`、`.bin` | バンク1のELFとTWDN画像 |
| `generated/widen-images.h` | 両画像のbyte配列、entry、build_id、SHA-256 |
| `generated/widen-manifest.json` | 画像hash、入力hash、5パッチ語、再生成情報 |

処理は契約v0.2.2 §6.1〜§6.3の候補で、固定coax
`b118c14f85e09ed0f98340a60dec0ae70aed3ea5` の
`internal/coaxd/usb/bridge/mb86h47/addressing.go:44-159` を根拠にする。

- 状態`+0x40`が非0なら、`R4 | (byte << 24)`を`+0x44`へ書く。書込み経路だけ
  `+0x40`を0にする。
- 0で`0x80000 <= R4 < 0xA0000`なら、`(R4 & 0x3FFFF) + 0xF00000`を`+0x44`へ書く。
- それ以外はアドレスを変えず、固定resume（read `0xE9AE6`、write `0xE9E2E`）へ戻る。
- 変更するレジスタはR0/R1/R4/R12だけ。`EXEC_READ`（`0xF0`以上）は実装しない。

生成は明示targetで行い、追跡ファイルを更新する。通常のbuildはこのtargetを
実行しない。

```sh
cmake -S . -B build-fr30 -G Ninja -DTORNE_FR30_BINUTILS_DIR=/path/to/binutils-build
cmake --build build-fr30 --target torne_fr30_widen
```

同じツールを指定した場合だけ、CTest `torne_widen_firmware` が登録される。
これは別ディレクトリで2回生成してbyte一致を確かめ、追跡済みの
`fr30/generated/`と比較する。あわせてTWDNのmagic/ABI/build_id/
capabilities/image_bytes/entry/reserved、ELF32・big endian・`EM_FR30`・
entry、4B整列、容量上限、誤header・範囲外entry・容量超過の拒否を検査する。
build_idは `+0x08` を0にした画像のSHA-256先頭4Bで、2回のリンクで
それ以外のbyteが変わらないことを確認する。契約v0.2.2のTWDNヘッダにはCRC欄が
ないため、整合はbuild_idとmanifestのSHA-256で確認する。

**この画像は実機で動かしていない。** ROMの5語・resume・R4/R8/R10の状態、
バンク1の未使用性・実容量、32bit/64B書込み、ROM互換性は固定source由来の
候補であり、独立した実機観測ではない。ホストloader、ROMパッチ適用、実機
ロードはこの増分に含めず、通常起動で自動ロードしない。

## 将来の配布FWの位置と生成経路

Widenは上記のとおり実ファイルを持つ。Core 0 / Core 1はまだ追加していない。
将来の対象も同じ配置に揃える。

```text
fr30/
├── README.md
├── src/widen.s                # 実装済み候補
├── src/<core-target>.s        # Core 0 / Core 1（未追加）
├── ld/widen-bank0.ld          # 実装済み
├── ld/widen-bank1.ld          # 実装済み
├── ld/<core-target>.ld        # 未追加
├── generated/widen-bank0.*    # 実装済みの追跡生成物
├── generated/widen-bank1.*    # 実装済みの追跡生成物
└── generated/widen-images.h   # 埋込用の両画像
```

- Coreは`core0`、`core1`を独立に持つ。同じローカル番地でも別の物理CPUなので
  別データにする。
- 生成は各対象の`.s`と`.ld`に固定binutilsを使い、`as --fatal-warnings` →
  `ld -m elf32fr30 -T ... -nostdlib` → `objcopy -O binary`の順で行う。
  生成物のSHA-256、入力hash、パッチ語をmanifestへ記録する。
- 製品CMakeは生成済み`.bin`だけを読み、big endianの32bit語として埋込用
  データへ変換する。ホストはWidenの選択バンク、またはCore別のローカル命令RAMへ
  ロードする。配布環境にクロスアセンブラを要求しない。
- 再生成は保守者の明示targetと専用CIで行い、commit済み生成物とのbyte一致を
  確認する。製品用の独立Makefileや独自assemblerは作らない。

## この増分で決めないこと

- Core 0 / Core 1の処理内容・ソース・画像
- Widenの実機動作、ROM互換性、バンク1の未使用性・実容量、32bit/64B書込み
- ROMパッチの適用、バンク切替手順、ホストloader
- 共有RAMの容量・配置、メールボックス/レジスタの確定値
- 生成済みFWをリリースへ含める時期と、実機段階試験の結果

## CI

`.github/workflows/fr30-generation.yml`は、固定ソースのハッシュと署名を確認し、
binutils 2.47をビルドしてfixture生成、Widen生成・追跡物比較、通常ビルド・CTestを
実行する定義である。この増分では定義を置くだけで、GitHub Actionsでの実行結果は
まだない。
