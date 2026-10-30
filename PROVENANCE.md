# 出典と依存

## 製品コード

`torne-userland` 本体は GPL-2.0-only。`LICENSE` を参照。

## dtv-userland-core

- repository: `https://github.com/Khronos31/dtv-userland-core.git`
- commit: `301c4593fc34702729750477d93f9d9e0051b0cb`（v1.0.1 の実体）
- 固定箇所: `cmake/TorneCoreDependency.cmake`
- 用途: `host_ctl`、`host_ts`、`identity`、`json`、`host_runtime`、
  `host_facade` の既存 API。製品側で共通契約を複製しない。
- ライセンス: Core リポジトリの `LICENSE` / `PROVENANCE.json` に従う。
  portable 部分は MIT、GPL adapter は GPL-2.0-only。
- この commit は現時点の再現可能な参照であり、Core 整理後の最終 pin ではない。

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
  set_configuration、非同期 transfer は使わない。
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
  `301c4593...`、MIT範囲）を使用する。外部runtime依存は増やしていない。
- `tests/widen_recognition_tests.cpp` に置く85語/340Bの旧画像は、登録tupleの
  受理・変異試験専用の固定参照データであり、製品FWではない。出典は同じ
  `torne_drv` commitの `torne_addressing0_code[85]`（GPL-2.0-only、coax/Linux由来）。
  製品のビルドやインストールでこのfixtureをFWとして使用しない。

## Widenソース

- `fr30/src/widen.s` と `fr30/ld/widen-bank*.ld` は torne-userland 用に
  書いた自作FR30ソース・リンカスクリプト。
- 処理の根拠: 採用契約v0.2.2 `docs/torne-fr30-host-contract-draft.md`
  §6.1〜§6.3、固定 `coax` commit
  `b118c14f85e09ed0f98340a60dec0ae70aed3ea5` の
  `internal/coaxd/usb/bridge/mb86h47/addressing.go:44-159`。
- 由来: 旧FW・denpaのコードや生成物をコピーせず、ROMハンドラのABI候補と
  分岐形式だけを固定sourceから参照した。実機ROM・動作の独立確認はない。
- 生成物: `fr30/generated/` の画像・hash・manifest。manifestにソース・
  linker script・binutils版・5パッチ語を記録する。通常起動では自動ロードしない。

## この増分で採用していないもの

- 旧 Draft PR #1 のコード、生成 FR30、`torne_fw_gen.h`。
- denpa / NeToRne のコード・生成物・調整値。
- Linux カーネルモジュール、usbfs/udev/IOKit/WinUSB の直接利用。
- USB control/bulk transport、FR30、DiB8096、カード UART。libusb は列挙だけに
  使い、機器の claim/初期化/転送には使わない。
