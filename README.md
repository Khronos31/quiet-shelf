# torne-userland

Sony CECH-ZD1 J（USB 054c:04b2）用の、カーネルドライバに依存しない ISDB-T チューナードライバ。

開発中です。ハードウェア対応・OS対応・受信品質の保証はまだありません。ライセンスは GPL-2.0-only です。

共通基盤には [dtv-userland-core](https://github.com/Khronos31/dtv-userland-core) v1.0.1 の固定コミット `301c4593fc34702729750477d93f9d9e0051b0cb` を暫定的に使用する。最終 pin は Core 側の整理後に5製品と揃える。固定箇所は `cmake/TorneCoreDependency.cmake`、出典は `PROVENANCE.md` に記録する。開発者向けの `TORNE_CORE_SOURCE_DIR` は未検証入力で、正式配布の入力監査とは別である。

正式コマンドは `torned`（ハードウェア所有デーモン）、`torne-ts`（TS出力）、`tornectl`（制御・診断）の3つです。現在は3コマンドの入口、Core 接続、静的 libusb による非侵襲 USB 列挙、USB transport（open/claim/control/bulk/close）、read-onlyのWiden識別、FR30機械語の生成・検査の土台とWiden候補画像までです。FR30の実機動作・ROM互換性、Core 0/1、DiB8096・カード・TS非同期・デーモンの listener は未実装または未検証です。`torned --list-json` は機器を open/claim せずに列挙し、serial は捏造しません。詳細は [docs/cli-framework.md](docs/cli-framework.md) を参照してください。FR30機械語の生成・検査、Widen候補画像と固定入力は [fr30/README.md](fr30/README.md) にあります。Widen候補は通常起動では自動ロードしません。

libusb は 1.0.30 を固定 URL/SHA-256 から取得し、udev 無効の静的ライブラリとしてリンクします（[PROVENANCE.md](PROVENANCE.md)）。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/torned --list-json
```
