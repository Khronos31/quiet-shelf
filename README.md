# torne-userland

Sony CECH-ZD1 J（USB 054c:04b2）用の、カーネルドライバに依存しない ISDB-T チューナードライバ。

開発中です。ハードウェア対応・OS対応・受信品質の保証はまだありません。ライセンスは GPL-2.0-only です。

共通基盤には [dtv-userland-core](https://github.com/Khronos31/dtv-userland-core) v1.0.1 の固定コミット `301c4593fc34702729750477d93f9d9e0051b0cb` を暫定的に使用する。最終 pin は Core 側の整理後に5製品と揃える。固定箇所は `cmake/TorneCoreDependency.cmake`、出典は `PROVENANCE.md` に記録する。開発者向けの `TORNE_CORE_SOURCE_DIR` は未検証入力で、正式配布の入力監査とは別である。

正式コマンドは `torned`（ハードウェア所有デーモン）、`torne-ts`（TS出力）、`tornectl`（制御・診断）の3つです。現在は3コマンドの入口、Core 接続、静的 libusb による非侵襲 USB 列挙、USB transport（open/claim/control/bulk/close）、read-onlyのWiden識別、Widen導入transaction（直接CLIで実機使用）、Core 1 host load/start runtimeと8slot I²C backend、Core 0 TS搬送候補画像とCore 0 host runtime/非同期Bulk/TS framer（直接CLIで使用、daemon未接続）、DiB8096 C11 frontend（backend注入、直接CLIで使用）、カードUART土台（typed EP0・presence/reset/FIFO・fake）、`torne-ts --device usb:`の直接ハードウェア入口（正式startup/Widen/Core/RF/TS）、FR30機械語の生成・検査の土台、Widen候補画像、Core 1 I²C候補画像までです。FR30画像のROM互換性の詳細、カードATR/T=1/CardService、daemon/family（PR18未収録）は未収録または未検証です。直接入口の実機T27/TSはユーザー承認済みの測定報告があり、CIは実機open/claim/tune/TSを行いません。`torned --list-json` は機器を open/claim せずに列挙し、serial は捏造しません。詳細は [docs/cli-framework.md](docs/cli-framework.md) を参照してください。FR30機械語の生成・検査、Widen候補画像と固定入力は [fr30/README.md](fr30/README.md) にあります。追跡済みWiden/Core 0/Core 1画像は通常buildでは生成も自動ロードもせず、直接CLIの起動経路が実機でWiden書込み/機能確認、Core起動、選局、TSを行う。daemon/familyはPR18未収録、カードはUART境界までです。

libusb は 1.0.30 を固定 URL/SHA-256 から取得し、udev 無効の静的ライブラリとしてリンクします（[PROVENANCE.md](PROVENANCE.md)）。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/torned --list-json
```
