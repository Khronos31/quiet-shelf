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
  `libusb_get_port_numbers`。いずれも公開 API。デバイスの open/claim/reset/
  transfer は行わない。
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

## この増分で採用していないもの

- 旧 Draft PR #1 のコード、生成 FR30、`torne_fw_gen.h`。
- denpa / NeToRne のコード・生成物・調整値。
- Linux カーネルモジュール、usbfs/udev/IOKit/WinUSB の直接利用。
- USB control/bulk transport、FR30、DiB8096、カード UART。libusb は列挙だけに
  使い、機器の claim/初期化/転送には使わない。
