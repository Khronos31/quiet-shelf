# torne-userland

Sony CECH-ZD1 J（USB 054c:04b2）用の、カーネルドライバに依存しない ISDB-T チューナードライバ。

開発中です。ハードウェア対応・OS対応・受信品質の保証はまだありません。ライセンスは GPL-2.0-only です。

共通基盤には [dtv-userland-core](https://github.com/Khronos31/dtv-userland-core) の開発pin `9d8e02c4f113568012e257ed46d72c9a55751958` を使用する。これはCard/T=1の開発branch用中間pinで、最終5製品同pin・製品main採用ではない。固定箇所は `cmake/TorneCoreDependency.cmake`、出典は `PROVENANCE.md` に記録する。開発者向けの `TORNE_CORE_SOURCE_DIR` は未検証入力で、正式配布の入力監査とは別である。

正式コマンドは `torned`（ハードウェア所有デーモン）、`torne-ts`（TS出力）、`tornectl`（制御・診断）の3つです。現在は3コマンドの入口、Core 接続、静的 libusb による非侵襲 USB 列挙、USB transport（open/claim/control/bulk/close）、read-onlyのWiden識別、Widen導入transaction（直接CLIで実機使用）、Core 1 host load/start runtimeと8slot I²C backend、Core 0 TS搬送候補画像とCore 0 host runtime/非同期Bulk/TS framer、DiB8096 C11 frontend（backend注入、直接CLIで使用）、カードUART土台（typed EP0・presence/reset/FIFO・fake）とCardService（Core9d8接続、fake/CTest）、`torne-ts --device usb:`の直接ハードウェア入口（正式startup/Widen/Core/RF/TS）、FR30機械語の生成・検査の土台、Widen候補画像、Core 1 I²C候補画像までです。FR30のROM互換性、Core 0/1の製品統合、DiB8096の実機backend接続・選局/TS、TSエンジン/USB DMA/EP、カードATR/T=1/CardServiceはCore9d8経由で通常daemonに接続済み（fake/CTest、実機I/O未検証）で、TS非同期/TsPumpもCore0経路に接続済み（fake/CTest、実機品質・長時間は未検証）です。Core listener は `torned` に接続済みですが実機未検証です。直接入口の実機T27/TSは測定担当が確認します。`torned --list-json` は機器を open/claim せずに列挙し、serial は捏造しません。詳細は [docs/cli-framework.md](docs/cli-framework.md) を参照してください。FR30機械語の生成・検査、Widen候補画像と固定入力は [fr30/README.md](fr30/README.md) にあります。追跡済みWiden/Core 0/Core 1画像は通常buildでは生成も自動ロードもせず、直接CLIの起動経路が実機でWiden書込み/機能確認、Core起動、選局、TSを行う。daemon/familyは収録済み（fake/CTest、実機未検証）、カードはUART/CardService境界までです。

libusb は 1.0.30 を固定 URL/SHA-256 から取得し、udev 無効の静的ライブラリとしてリンクします（[PROVENANCE.md](PROVENANCE.md)）。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/torned --list-json
```
