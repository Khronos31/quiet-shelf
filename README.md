# torne-userland

Sony CECH-ZD1 J（USB 054c:04b2）用の、カーネルドライバに依存しない ISDB-T チューナードライバ。

開発中です。ハードウェア対応・OS対応・受信品質の保証はまだありません。ライセンスは GPL-2.0-only です。

共通基盤には [dtv-userland-core](https://github.com/Khronos31/dtv-userland-core) の採用中間pin `1d5569ee303204cbb2d660517152d8a5b5878041`（Core141）を使用する。これはUnix IFDのtorne profileを含む中間pinで、最終5製品同pin・tagではない。固定箇所は `cmake/TorneCoreDependency.cmake`、出典は `PROVENANCE.md` に記録する。開発者向けの `TORNE_CORE_SOURCE_DIR` は未検証入力で、正式配布の入力監査とは別である。

正式コマンドは `torned`（ハードウェア所有デーモン）、`torne-ts`（TS出力）、`tornectl`（制御・診断）の3つです。現在は3コマンドの入口、Core 接続、静的 libusb による非侵襲 USB 列挙、USB transport（open/claim/control/bulk/close）、read-onlyのWiden識別、Widen導入transaction（直接CLIで実機使用）、Core 1 host load/start runtimeと8slot I²C backend、Core 0 TS搬送候補画像とCore 0 host runtime/非同期Bulk/TS framer、DiB8096 C11 frontend（backend注入、直接CLIで使用）、カードUART土台（typed EP0・presence/reset/FIFO・fake）とCardService（Core141接続、fake/CTest）、`torne-ts --device usb:`の直接ハードウェア入口（正式startup/Widen/Core/RF/TS）、FR30機械語の生成・検査の土台、Widen候補画像、Core 1 I²C候補画像までです。FR30のROM互換性、Core 0/1の製品統合、DiB8096の実機backend接続・選局/TS、TSエンジン/USB DMA/EP、カードATR/T=1/CardServiceはCore141経由で通常daemonに接続済み（fake/CTest、実機I/O未検証）で、TS非同期/TsPumpもCore0経路に接続済み（fake/CTest、実機品質・長時間は未検証）です。Core listener は `torned` に接続済みですが実機未検証です。直接入口の実機T27/TSは測定担当が確認します。`torned --list-json` は機器を open/claim せずに列挙し、serial は捏造しません。詳細は [docs/cli-framework.md](docs/cli-framework.md) を参照してください。FR30機械語の生成・検査、Widen候補画像と固定入力は [fr30/README.md](fr30/README.md) にあります。追跡済みWiden/Core 0/Core 1画像は通常buildでは生成も自動ロードもせず、直接CLIの起動経路が実機でWiden書込み/機能確認、Core起動、選局、TSを行う。daemon/familyは収録済み（fake/CTest、実機未検証）、カードはUART/CardService境界までです。

libusb は 1.0.30 を固定 URL/SHA-256 から取得し、udev 無効の静的ライブラリとしてリンクします（[PROVENANCE.md](PROVENANCE.md)）。

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/torned --list-json
```

## Optional PC/SC packaging

The Core PC/SC adapters are optional packaging inputs; the normal daemon and
CLI do not require `ifdhandler.h`.

- Windows (`TORNE_BUILD_WINSCARD=ON`, the default on Windows): builds the
  shared Core `WinSCard.dll` and stages it with the GPL adapter `COPYING` and
  README, and the Core MIT `LICENSE.dtv-userland-core` under
  `build/pcsc/windows/` via the `torne_pcsc_stage` target. On the MinGW
  toolchain the statically linked MinGW-w64 runtime notice
  `COPYING.MinGW-w64-runtime.txt` is also staged; MSVC does not add that
  runtime notice. The same notices are installed under the application's
  license directory. The staged DLL belongs
  beside the application; it is not a System32 replacement and this project
  does not register OS readers.
- Linux/macOS (`TORNE_BUILD_PCSC_IFD=OFF` by default): enabling it sets the
  Core `DTV_USERLAND_BUILD_UNIX_PCSC_IFD=ON` and
  `DTV_USERLAND_UNIX_PCSC_TORNE_PROFILE=ON` options from the fixed Core141
  input. `torne_pcsc_stage` stages the Unix IFD shared library, Core adapter
  `COPYING`/provenance, Core MIT `LICENSE.dtv-userland-core`, and the reader
  configuration template. It never falls
  back to the PX4 profile; ASICEN and torne profiles remain exclusive.
  `packaging/pcsc/torne.reader.conf.in` is a reader configuration template
  only. `packaging/macos/Info.plist.in` and `packaging/macos/CMakeLists.txt`
  stage a candidate bundle with Core adapter `COPYING`/provenance and Core MIT
  `LICENSE.dtv-userland-core`; macOS loader/registration and the Linux IFD ABI
  connection remain unverified.
- No packaging target edits `/etc`, starts pcscd, installs a system service, or
  replaces a system DLL. The Windows DLL is installed beside the application
  when the product install target is used.

The Core MIT foundation and the GPL-2.0-only PC/SC adapters are separate; the
adapter licenses and provenance are carried with the staged artifacts.
