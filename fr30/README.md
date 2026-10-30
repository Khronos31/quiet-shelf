# FR30生成の土台

このディレクトリは、製品に将来同梱するFR30機械語を、固定したGNU binutilsと
自作ソースから再生成・検査するための土台を示す。**この増分にはWidenの
read/write handler候補と両bank画像、Core 1 I²CとCore 0 TS搬送の候補ソース・
画像がある。通常buildは画像を生成も自動ロードもしない。直接CLIの起動経路は実機でWiden/Core/RF/TSを実行する。CIは実機open/claim/tune/TSを行わない。**
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
cmake --build build-fr30 --target torne_fr30_widen
cmake --build build-fr30 --target torne_fr30_core1
cmake --build build-fr30 --target torne_fr30_core0
```

同じツールを指定してconfigureした場合だけ、`torne_fr30_generation`、
`torne_widen_firmware`、`torne_core1_firmware`、`torne_core0_firmware`テストが
CTestへ登録される。
各targetとテストは対応する生成scriptで、2つの独立した出力ディレクトリへ
生成してbyte一致を確認する。

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

**通常build/CIはこの画像を実機実行しない。** ROMの5語・resume・R4/R8/R10の状態、
バンク1の未使用性・実容量、32bit/64B書込み、ROM互換性の詳細は固定source由来の
候補であり、独立した実機検証範囲は測定担当の報告に従う。直接CLIの起動経路は
追跡画像を実機で使用し、Widen導入/機能確認を行う。通常buildは画像を生成も
自動ロードもしない。CIはopen/claim/tune/TSを行わない。

## Core 1 I²C候補

`fr30/src/core1.s` は、契約v0.2.2のCore 1 I²C初版を実装する自作FR30候補である。`fr30/ld/core1.ld` がコードを`0xA0000`へ置き、code+const 12KiB、stack `0xA3000`、shared data `0xA3800`、4B整列、entryをASSERTする。`fr30/generated/` に追跡する。

| ファイル | 内容 |
| --- | --- |
| `src/core1.s` | 起動INFO、CTRL/STAT、session/heartbeat、NOP ack、8slot ring、byte I²C、HALTED/resume |
| `ld/core1.ld` | entry `0xA0000`、12KiB上限、stack/shared非重複、4B整列 |
| `generated/core1.elf`、`.bin` | ELF32/BE/EM_FR30とraw image |
| `generated/core1-images.h` | byte配列、entry、build_id、SHA-256 |
| `generated/core1-manifest.json` | source/linker/tool、画像hash、配置 |

処理は契約§2.6/§3/§5/§7/§9.1の候補で、byte制御値・poll上限は固定coax `b118c14f` の`core1prog.go:28-113`と`regs/usb.go`に由来する。

- INFO: magic `0x54493243`を最後、ABI `0x00010000`、build_id、`I2C_RING`、session echo、image_bytes、data_base `0xA3800`、param0 `0x40000`。
- CTRL/STAT: session、NOP `cmd_seq`/`cmd_ack_seq`、heartbeat、8slot I²C ring、`i2c_progress`のbit31 HALTED、HALTED/resume。
- I²C: busy待ち、START/repeated START、ACK/NACK、READ/WRITE/WRITE_READ、最大32B、STOP後idle、byte poll `0x40000`。ADDR/DATA NACK、BUS_ERROR、TIMEOUT、BUS_BUSY、INVALID、TORNを結果コードと段階で返す。
- HALTEDでは新規要求を取らず、CONTINUE/DISCARDを`i2c_resume`のindex/action/serialで処理する。DISCARDは受理時の`CTRL.i2c_req_prod`で対象範囲を固定し、全SKIPPED完了を公開するまで`STAT.state`と`i2c_progress`のHALTED bitを維持し、`resume_ack`も完了後まで更新しない。CONTINUEも解除の公開後にackを出す。
- hostのS4（I²C master初期化）はCoreへ移していない。通常buildは追跡画像だけを消費する。通常daemonの`torned --device`起動はCore 1 HOLD/S4/load/RUNとCore 0を接続する（fake/CTestまで、実機I/Oは測定担当）。CIは実機open/claim/RUN/TSを行わない。

生成は明示targetで行う。通常buildは追跡画像を消費するだけで、クロスアセンブラを要求しない。

```sh
cmake -S . -B build-fr30 -G Ninja -DTORNE_FR30_BINUTILS_DIR=/path/to/binutils-build
cmake --build build-fr30 --target torne_fr30_core1
```

ツール指定時だけCTest `torne_core1_firmware`が登録され、別出力で2回生成してbyte一致、追跡物一致、ELF32/BE/EM_FR30/entry `0xA0000`、4B整列、12KiB上限、build_idの0欄hash、容量超過・不正entryの拒否を確認する。`tests/core1_model.py`は開発者向けの任意モデルで、boot/INFO、WRITE/READ/WRITE_READ、32B、TORN、ADDR/DATA NACK、CONTINUE/DISCARDを確認する。CTestの必須条件ではない。

**通常build/CIはこの画像を実機実行しない。** 直接CLIの起動経路は追跡画像を実機で使用する。CIはopen/claim/tune/TSを行わない。I²C MMIO値、byte poll時間、`A3800-A3FFF`のRAM実在・連続性、HOLD/RUN/vector、Core 1のNOP/I²Cの詳細確認は測定担当の報告に従う。

## Core 0 TS搬送候補

`fr30/src/core0.s` は、契約v0.2.2 §2/§4/§5/§7/§9.1のCore 0初版を実装する自作FR30候補である。`fr30/ld/core0.ld` がコードを`0xA0000`へ置き、code+const 12KiB、stack `0xA3000`、shared data `0xA3800`、4B整列、entryをASSERTする。`fr30/generated/` に追跡する。

| ファイル | 内容 |
| --- | --- |
| `src/core0.s` | 起動INFO/CTRL/STAT、session/epoch/heartbeat/NOP/FAULT、内部DMA停止の有界待ち、FIFO/refill/packetizer、writer limit更新 |
| `ld/core0.ld` | entry `0xA0000`、12KiB上限、stack/shared非重複、4B整列 |
| `generated/core0.elf`、`.bin` | ELF32/BE/EM_FR30とraw image |
| `generated/core0-images.h` | byte配列、entry、build_id、SHA-256 |
| `generated/core0-manifest.json` | source/linker/tool、画像hash、配置 |

処理は契約§2.6/§4/§5/§7/§9.1の候補で、内部DMA/FIFO/packetizerの初期値とevent bitは固定coax `b118c14f` の`core0prog.go:25-137`と`stream.go:160-173`に由来する。

- INFO: magic `0x54545330`を最後、ABI `0x00010000`、build_id、`TS_RING`、session echo、image_bytes、data_base `0xA3800`、param0 FIFO容量 `0x840`。
- CTRL: `session`の上位16bit非0、`stream_epoch`、`ring_base`/`ring_size`/`unit_size=192`、`writer_offset_reg`/`read_limit_reg`、`margin`、`report_every`を起動時に一度読み、不正なら`F_BAD_CTRL`でFAULT。
- BOOT: 内部DMAの停止要求を`0x1068 & 0x300`で確認する。待ちは実装上の回数上限を持ち、超えたら`F_DMA_QUIESCE`でFAULT。現在のwriter offsetを読み位置と読み上限の初期値にする。
- ループ: heartbeat、NOP応答、event bit29のrefill完了ACK、writer offsetからDMA読み上限を更新、FIFO空きが`0x470`を超えたら`0x460`補充、event bit5とpacketizer idleで192B単位を送出、`report_every`単位ごとに読取り位置−`margin`をwriter limitへ反映、`max_fill`/`near_full`を更新。
- STAT: `epoch_echo`をRUNNING前に書き、`units_sent`/`refills`/`last_event`/`write_off`/`read_off`/水位を診断用に公開する。FAULT後はデータ経路へ触れず、heartbeatとNOP応答だけを続ける。
- 初期補充はSTATの`refills`に数えない（固定sourceのdebug counterに合わせる）。refillの完了はevent bit29で受ける。

生成は明示targetで行う。通常buildは追跡画像を消費するだけで、クロスアセンブラを要求しない。

```sh
cmake -S . -B build-fr30 -G Ninja -DTORNE_FR30_BINUTILS_DIR=/path/to/binutils-build
cmake --build build-fr30 --target torne_fr30_core0
```

ツール指定時だけCTest `torne_core0_firmware`が登録され、別出力で2回生成してbyte一致、追跡物一致、ELF32/BE/EM_FR30/entry `0xA0000`、4B整列、12KiB上限、build_idの0欄hash、容量超過・不正entryの拒否を確認する。`tests/core0_model.py`は開発者向けの任意モデルで、boot/INFO、不正CTRL、DMA停止不能、ring wrap、refill、192B送出、10単位のlimit更新を確認する。CTestの必須条件ではない。

**通常build/CIはこの画像を実機実行しない。** 直接CLIの起動経路は追跡画像を実機で使用する。CIはopen/claim/tune/TSを行わない。内部DMA/FIFO/packetizer、SDRAMリング、192B→188B、writer limit、HOLD後のDMA挙動の詳細確認は測定担当の報告に従う。

## 将来の配布FWの位置と生成経路

Widen、Core 1、Core 0は上記のとおり実ファイルを持つ。
将来の対象も同じ配置に揃える。

```text
fr30/
├── README.md
├── src/widen.s                # 実装済み候補
├── src/core1.s                # 実装済み候補
├── src/core0.s                # 実装済み候補
├── ld/widen-bank0.ld          # 実装済み
├── ld/widen-bank1.ld          # 実装済み
├── ld/core1.ld                # 実装済み
├── ld/core0.ld                # 実装済み
├── generated/widen-bank0.*    # 実装済みの追跡生成物
├── generated/widen-bank1.*    # 実装済みの追跡生成物
├── generated/core1.*          # 実装済みの追跡生成物
├── generated/core0.*          # 実装済みの追跡生成物
└── generated/widen-images.h   # 埋込用のWiden両画像
```

- Core 0とCore 1は独立に持つ。同じローカル番地でも別の物理CPUなので
  別データにする。どちらも実装済み候補で、直接CLIの起動経路が実機で使用する。詳細確認は別工程。
- 生成は各対象の`.s`と`.ld`に固定binutilsを使い、`as --fatal-warnings` →
  `ld -m elf32fr30 -T ... -nostdlib` → `objcopy -O binary`の順で行う。
  生成物のSHA-256、入力hash、パッチ語をmanifestへ記録する。
- 製品CMakeは生成済み`.bin`だけを読み、big endianの32bit語として埋込用
  データへ変換する。ホストはWidenの選択バンク、またはCore別のローカル命令RAMへ
  ロードする。配布環境にクロスアセンブラを要求しない。
- 再生成は保守者の明示targetと専用CIで行い、commit済み生成物とのbyte一致を
  確認する。製品用の独立Makefileや独自assemblerは作らない。

## この増分で決めないこと

- Core 0/1のDMA/FIFO/packetizer・I²C MMIO/poll・Core RAM連続性・HOLD/RUN/vectorの詳細確認
- Widen bank1の未使用性・実容量、32bit/64B書込み、ROM互換性の詳細
- V-W1/V-W2/V-C0/V-C1、物理復旧、実機段階試験の結果
- 共有RAMの容量・配置、メールボックス/レジスタの確定値
- 生成済みFWをリリースへ含める時期と、実機段階試験の結果

## CI

`.github/workflows/fr30-generation.yml`は、固定ソースのハッシュと署名を確認し、
binutils 2.47をビルドしてfixture生成、Widen生成・追跡物比較、通常ビルド・CTestを
実行する定義である。この増分では定義を置くだけで、GitHub Actionsでの実行結果は
まだない。
