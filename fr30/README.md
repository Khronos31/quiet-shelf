# FR30生成の土台

このディレクトリは、製品に将来同梱するFR30機械語を、固定したGNU binutilsと
自作ソースから再生成・検査するための土台を示す。**この増分には製品用の
Widen / Core 0 / Core 1ファームウェアも、ROMパッチ語も、共有RAM配置もない。**
通常のCMakeビルドと配布物は、クロスアセンブラを要求しない。

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

## 将来の配布FWの位置と生成経路

製品FWを追加する増分では、次の構成を置く。**まだ実ファイルは作っていない。**

```text
fr30/
├── README.md                  # この文書
├── src/<target>.s             # 自作アセンブリ（Widen / Core 0 / Core 1）
├── ld/<target>.ld             # 対象別の配置・容量検査
└── generated/<target>.bin     # 生成物と<hash>（配布物へ同梱）
```

- `<target>`は`widen-bank0`、`widen-bank1`、`core0`、`core1`を独立に持つ。
  同じローカル番地でもCore 0とCore 1は別の物理CPUなので別データにする。
- `cmake/fr30_generate.cmake`を対象別ソースへ拡張し、固定binutilsで
  `as --fatal-warnings` → `ld -m elf32fr30 -T ... -nostdlib` →
  `objcopy -O binary`を実行する。生成物のSHA-256と入力版を記録する。
- 製品CMakeは生成済み`.bin`だけを読み、big endianの32bit語として埋込用
  データへ変換する。ホストはWidenの選択バンク、またはCore別のローカル命令RAMへ
  ロードする。配布環境にクロスアセンブラを要求しない。
- 再生成は保守者の明示targetと専用CIで行い、commit済み生成物とのbyte一致を
  確認する。製品用の独立Makefileや独自assemblerは作らない。

## この増分で決めないこと

- 実際のWiden / Core 0 / Core 1の処理内容、使用命令一覧、実機動作
- ROM側の変更箇所・パッチ語・期待元語、バンク切替手順
- 共有RAMの容量・配置、メールボックス/レジスタの確定値
- 生成済みFWをリリースへ含める時期と、実機段階試験の結果

## CI

`.github/workflows/fr30-generation.yml`は、固定ソースのハッシュと署名を確認し、
binutils 2.47をビルドしてfixture生成と通常ビルド・CTestを実行する定義である。
この増分では定義を置くだけで、GitHub Actionsでの実行結果はまだない。
