# 出典と依存

## 製品コード

`torne-userland` 本体は GPL-2.0-only。`LICENSE` を参照。

## dtv-userland-core

- repository: `https://github.com/Khronos31/dtv-userland-core.git`
- commit: `9d8e02c4f113568012e257ed46d72c9a55751958`（Card/T=1開発branch用の中間pin）
- 固定箇所: `cmake/TorneCoreDependency.cmake`
- 用途: `host_ctl`、`host_ts`、`identity`、`json`、`host_runtime`、
  `host_facade`、`host_card_t1` の既存 API。製品側でATR/T=1/APDU契約を複製しない。
- ライセンス: Core リポジトリの `LICENSE` / `PROVENANCE.json` に従う。
  portable 部分は MIT、GPL adapter は GPL-2.0-only。
- この commit はCardService接続のための中間pinであり、最終5製品同pinではない。

Core のソースは FetchContent で取得するか、`TORNE_CORE_SOURCE_DIR` で同じ
commit を checkout 済みのツリーを指定する。生成物や取得物はこのリポジトリへ
vendor しない。

- 既定の FetchContent 経路は `cmake/TorneCoreDependency.cmake` の固定 commit を
  取得する。
- `TORNE_CORE_SOURCE_DIR` は開発者向けの未検証入力である。CMake は指定ツリーの
  commit・内容・ハッシュを検証しない。正式配布物の入力監査（固定版、ハッシュ、
  対応ソース）は別工程で行う。

## libusb

- version: `1.0.30`
- source: `https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2`
- SHA-256: `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`
- 固定箇所: `cmake/TorneLibusb.cmake`
- ビルド: libusb 自身の Autotools。`--disable-shared --enable-static
  --disable-udev --disable-tests-build --disable-examples-build`。製品用の別
  Makefile は持たない。ソースはビルド時に取得し、vendor しない。
- ライセンス: LGPL-2.1-or-later（libusb の `COPYING`）。製品の GPL-2.0-only
  実行ファイルへ静的リンクするため、配布時は LGPL の対応ソース・再リンク可能性・
  表示義務を別途満たす必要がある。今回の増分はソース内のリンクまでで、配布物の
  義務履行を完了したとは主張しない。
- 使用 API: `libusb_init`、`libusb_exit`、`libusb_get_device_list`、
  `libusb_free_device_list`、`libusb_get_device_descriptor`、
  `libusb_get_bus_number`、`libusb_get_device_address`、
  `libusb_get_port_numbers`、`libusb_open`、`libusb_close`、
  `libusb_get_configuration`、`libusb_get_active_config_descriptor`、
  `libusb_free_config_descriptor`、`libusb_claim_interface`、
  `libusb_release_interface`、`libusb_control_transfer`、
  `libusb_bulk_transfer`。いずれも公開 API。kernel driver の detach、reset、
  set_configuration は使わない。Core0 TsPumpは公開非同期API
  （`libusb_alloc_transfer`/`libusb_submit_transfer`/`libusb_cancel_transfer`/
  `libusb_handle_events_timeout_completed`）でbounded async bulkを受ける。
- udev は無効化している。Linux の最終 ELF に libusb/libusb-1.0.so/libudev の
  動的依存が無いことを検査する。
- CMake の cross target/sysroot/compiler 引数は libusb の configure へ渡す。
  Darwin では libusb 1.0.30 の `configure.ac` が `LIBS` に入れる `-lobjc` と
  IOKit/CoreFoundation/Security の framework を、静的アーカイブに残らないため
  `torne_libusb` の INTERFACE から最終リンクへ伝播する。

## FR30生成ツール

- ツール: GNU binutils 2.47（`fr30-elf`ターゲット）
- 配布元: `https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz`
- SHA-256: `154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff`
- 署名: Nick Clifton、`3A24BC1E8FB409FA9F14371813FCEF89DD9E3C4F`
- 用途: `tests/fixtures/fr30/`の自作命令例からの生成・再生成一致検査
- 配布: binutils自体を製品のruntime依存・同梱物にしない。生成手順、
  固定版、ハッシュ、自作ソースは[fr30/README.md](fr30/README.md)に記録する。

## Widen識別データ

- 登録済み旧coax bank0の識別tuple（`bank_base=0xE1000`、`image_bytes=340`、
  `entry_read=0xE1000`、`entry_write=0xE1076`、全画像SHA-256
  `6937805e74bf0ecc8f2a9e76750d4bd87ed2d53e85dbfb29376c2bc1145268e7`、
  5パッチ語）は、契約v0.2.2と固定
  `torne_drv` `fc662902a3e4ad56566b00cae4a0ead922e3dd02`
  `driver/torne_fw_gen.h:221-245` に基づく。製品コードはhash/entry/語だけを保持し、
  旧画像のbyte列を製品FWやロード対象へ埋め込まない。
- SHA-256はCoreの `include/dtv/userland/detail/sha256.h`（Core入力
  `9d8e02c4...`、MIT範囲）を使用する。外部runtime依存は増やしていない。
- `tests/widen_recognition_tests.cpp` に置く85語/340Bの旧画像は、登録tupleの
  受理・変異試験専用の固定参照データであり、製品FWではない。出典は同じ
  `torne_drv` commitの `torne_addressing0_code[85]`（GPL-2.0-only、coax/Linux由来）。
  製品のビルドやインストールでこのfixtureをFWとして使用しない。

## Widenソース

- `fr30/src/widen.s` と `fr30/ld/widen-bank*.ld` は torne-userland 用に
  書いた自作FR30ソース・リンカスクリプト。
- 処理の根拠: 採用契約v0.2.2
  §6.1〜§6.3、固定 `coax` commit
  `b118c14f85e09ed0f98340a60dec0ae70aed3ea5` の
  `internal/coaxd/usb/bridge/mb86h47/addressing.go:44-159`。
- 由来: 旧FW・denpaのコードや生成物をコピーせず、ROMハンドラのABI候補と
  分岐形式だけを固定sourceから参照した。実機ROM・動作の独立確認はない。
- 生成物: `fr30/generated/` の画像・hash・manifest。manifestにソース・
  linker script・binutils版・5パッチ語を記録する。通常buildは画像を生成も自動ロードもしない。直接CLIの起動経路が追跡画像を実機で使用する。

## Core 1ソース

- `fr30/src/core1.s` と `fr30/ld/core1.ld` は torne-userland 用に書いた
  自作FR30ソース・リンカスクリプト。
- 根拠: 採用契約v0.2.2 §2/§3/§5/§7/§9.1。
  byte制御値・poll上限は固定 `coax` commit
  `b118c14f85e09ed0f98340a60dec0ae70aed3ea5` の
  `internal/coaxd/usb/bridge/mb86h47/core1prog.go:28-113` と
  `internal/coaxd/usb/bridge/mb86h47/regs/usb.go`。
- 由来: 旧coax 16語ring/旧mailbox、denpaのAGPLコード/生成FW、旧生成画像を
  使わず、v0.2.2のwire・tag・slot・HALTED/resumeを実装した。直接CLIの起動経路はCore 1を起動しI²C backendへ接続する。詳細な実機検証範囲は測定担当の報告に従う。
- 生成物: `fr30/generated/core1.*`。source/linker/tool/hashをmanifestへ記録し、
  通常buildは画像を生成も自動ロードもしない。直接CLIの起動経路が追跡画像を実機で使用する。`tests/core1_model.py` は任意の開発者向け
  命令モデルで、製品の必須依存ではない。

## Core 0ソース

- `fr30/src/core0.s` と `fr30/ld/core0.ld` は torne-userland 用に書いた
  自作FR30ソース・リンカスクリプト。
- 根拠: 採用契約v0.2.2 §2/§4/§5/§7/§9.1。内部DMA/FIFO/packetizerの
  初期値とevent bitは固定 `coax` commit
  `b118c14f85e09ed0f98340a60dec0ae70aed3ea5` の
  `internal/coaxd/usb/bridge/mb86h47/core0prog.go:25-137` と
  `internal/coaxd/usb/bridge/mb86h47/stream.go:160-173`。
- 由来: 旧coaxの生成済みFR30語、独自counter/mailbox wire、denpaのAGPLコードと
  生成FWを転用せず、v0.2.2のINFO/CTRL/STAT、epoch、writer limit、refill、
  packetizer送出を実装した。固定sourceの無限DMA停止待ちは採用せず、回数上限を
  持つ`F_DMA_QUIESCE`へ置き換えた。直接CLIの起動経路はCore 0を起動してTSを搬送する。詳細な実機検証範囲は測定担当の報告に従う。
- 生成物: `fr30/generated/core0.*`。source/linker/tool/hashをmanifestへ記録し、
  通常buildは画像を生成も自動ロードもしない。直接CLIの起動経路が追跡画像を実機で使用する。`tests/core0_model.py` は任意の開発者向け
  命令モデルで、製品の必須依存ではない。

## Widen loader（host）

- `src/widen/loader.h`、`src/widen/loader.cpp`、拡張した`src/widen/bridge.*`は
  torne-userland用の自作hostコード。
- 根拠: 採用契約v0.2.2 §5.3/§6/§7。
- 範囲: 契約§6.3の導入transaction、5語/画像のreadback、32bit最大64B bridge、
  旧24bit経路、失敗停止。loader単体の試験はfakeで行い、直接CLIの起動経路が実機USBでopen/claim・Widen導入を行う。
- 由来: PR6の識別APIとPR7の自作TWDN画像/manifestを使用する。旧FWやdenpaの
  生成物をロード対象・rollback先にしない。

## Core1 I2C backendの実測入力

- D896=`0x01B3`、D897=`0x8002`、T1A ADDR_NACK、DISCARD後のD896再読、request/completionのbyte列は、実測に基づく。
- prototypeはwire/順序の参照であり、製品コードへコピーしていない。Core1 FWの生成headerには依存しない。
- 追加の外部runtime依存はない。S4/clock/GPIO初期化はbackendの前提で、実測値の自動初期化は行わない。

## Core0 TS host runtimeの入力

- TS engine/USB DMA/EP1/Core0配置の値と順序は、契約v0.2.2 §2/§4と固定coax `b118c14f...` の `stream.go`/`core0prog.go`、torne_drv `fc662902...` の `torne_bridge.c` を参照した。coaxはGPL-2.0-or-later側で利用し、生成FW・旧mailbox・denpaコードは取り込まない。
- C0-A画像は注入境界で受け、生成headerに依存しない。テストの画像は合成fixtureで、製品FWではない。

## カードUART土台（host）

- `src/card/uart.*` は torne-userland 用に書いた自作hostコード。ATR構文、T=1、
  APDUは実装せず、共有Coreの実在APIへ後で写像するUART境界だけを持つ。
- 根拠: 採用FR30契約
  §7.1、製品仕様CARD-01〜08。UART番地・冷reset順・10/2/1ms・初期9600と
  19200/38400のbaud候補は、固定 `torne_drv`
  `fc662902a3e4ad56566b00cae4a0ead922e3dd02` の `driver/torne_card.c:11-39,
  114-164` と固定 `coax` `b118c14f85e09ed0f98340a60dec0ae70aed3ea5` の
  `internal/coaxd/usb/bridge/mb86h47/card.go:13-51,92-184`。
- 由来: 両固定sourceはGPL互換。旧mailbox経由のカード命令、別USB claim、
  T=1の複製、カード専用FR30画像は使わない。`torne_drv`由来NOTICEは既存の
  `NOTICE`をそのまま保持する。
- 境界: RXはCONSUMING_READ、TXはFIFO_WRITE、SEND/FIFO reset/IRQ clearは
  TRIGGERとして再送しない。不正・short・unknown後はセッションを無効化し、
  後続操作を止める。実カード、presence極性、FIFO上限、byte lane、ATR/T=1は
  未実測。fake EP0/clockのみで検証する。
- `src/card/service.*` は製品UARTを共有Core **9d8e02c4** の公開
  `CardHardware`/`CardTime`/`CardSession`/`CardProtocolSessionAdapter`へ接続する
  自作host adapter。ATR構文・T=1・IFSD・EDC・R/S/WTX・APDUはCore既存APIに任せ、
  製品側で複製しない。`src/product/hardware_device.cpp`が同じ単一Transportから
  `CardPort`を公開し、`src/product/core_family.cpp`が`EnclosureFamily`の
  card backend/protocolとして`CardService`へ渡す。
- Core 9d8はMIT/GPL-2.0-onlyの既存Core sourceであり、製品は公開APIだけに依存
  する。Coreの生成物や他製品sourceをvendorしない。

## DiB8096 C11 frontend

- 元実装: `torne_drv` commit `fc662902a3e4ad56566b00cae4a0ead922e3dd02`
  の `driver/torne_dib.c`、`driver/torne_dib.h`、`driver/torne_core.h`、
  `driver/torne_dib_tables_gen.h`（GPL-2.0-only）。
- 元の系譜: Linux `dib8000` / `dib0090` / `dibx000_common`
  （Copyright (C) 2005-9 DiBcom, SA et al）と、coax
  `b118c14f85e09ed0f98340a60dec0ae70aed3ea5` のGPL-2.0-only側。
- 変更: `src/dib/`へ配置し、kernel向け分岐を使わずC11でコンパイル。
  `torne_core.h`へ注入式`torne_i2c_backend`とtimeoutを追加。GNUの`?:`を
  標準C三項演算子へ置換。未使用引数の明示。`src/dib/torne_i2c_adapter.c`と
  `src/dib/dib_frontend.c`は、submit/wait、32B/BE、flush、sleep/clock/log境界と
  Core1/GPIO前提を追加する自作コード。
- 生成表: `torne_dib_tables_gen.h`はcoax/Linux由来の係数表。denpaのAGPLコード、
  生成FW、旧FR30/ROM mailbox、vendor依存は使っていない。
- 実機境界: 実測T27はcoax GPL実装を使用した。このC11 frontendの
  直接CLIの起動経路はDiB init/tune/statusを実機で行う。詳細な実機検証範囲は測定担当の報告に従う。
- 上流表示: 固定`torne_drv`の`NOTICE`を`NOTICE`へ保存する。上流NOTICEのpx4_drv節は本snapshotに含まれない上流ファイルへの表示であり、px4_drv由来sourceは収録していない。

## この増分で採用していないもの

- 旧 Draft PR #1 のコード、生成 FR30、`torne_fw_gen.h`。
- denpa / NeToRne のコード・生成物・調整値。
- Linux カーネルモジュール、usbfs/udev/IOKit/WinUSB の直接利用。
- 実機USBでのdaemon/listener起動とカードATR/T=1/CardService I/O（sourceは収録し、fake/CTestで確認する）。
- CIでの実USB open/claim/tune/TS。通常buildはFR30画像を生成せず、追跡済み画像を使用する。
