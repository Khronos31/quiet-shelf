# torne-userland

Sony CECH-ZD1 J（USB 054c:04b2）用の、カーネルドライバに依存しない ISDB-T チューナードライバ。

開発中です。ハードウェア対応・OS対応・受信品質の保証はまだありません。ライセンスは GPL-2.0-only です。

共通基盤には [dtv-userland-core](https://github.com/Khronos31/dtv-userland-core) の採用pin `548a04605d9e224be179547de30f590ff693a128`（Core main、PR144採用）を使用する。これはWindows共通WinSCard SDK import fixtureとUnix IFDのtorne profileを含む採用pinで、最終5製品同pin・tagではない。固定箇所は `cmake/TorneCoreDependency.cmake`、出典は `PROVENANCE.md` に記録する。開発者向けの `TORNE_CORE_SOURCE_DIR` は未検証入力で、正式配布の入力監査とは別である。

正式コマンドは `torned`（ハードウェア所有デーモン）、`torne-ts`（TS出力）、`tornectl`（制御・診断）の3つです。静的 libusb の非侵襲列挙、USB transport（open/claim/control/bulk/close）、Widen識別と導入policy、Core 1 host/I²C、DiB8096 C11 frontend、Core 0 host/非同期Bulk/TS framer、カードUART/CardService、Core listener/daemon、`torne-ts --device usb:`の直接入口を製品buildへ接続済みです。`torned` は同じDeviceを`EnclosureFamily`/`EnclosureContext`へ渡し、`tornectl`と`torne-ts --instance`はCore IPCを使います。Linuxの既存実測では通常daemonのT27→T25が同一daemonでexit0、自作Core0 fenceは120/120、隔離した同一UID pcscdの標準APIでreader/ATR/T=1/APDU、Mac IFDのbundle load、Windows stage DLLのnative mock/SDK importまで確認しています。初期daemon T27のpoll/close失敗は修正後の後続T27/T25成功と区別して記録し、初期失敗だけを現在の未実装扱いしません。

未検証・未整備は、OS常設pcscd/別UID、Mac/Windows実機のカードI/O、F4の各OS受入、配布archive、FR30 ROM互換性・cold/rollbackの全条件です。`torned --list-json` は機器をopen/claimせずに列挙し、serialを捏造しません。詳細は [docs/cli-framework.md](docs/cli-framework.md)、FR30の生成・固定入力は [fr30/README.md](fr30/README.md) を参照してください。追跡Widen/Core画像は通常buildで生成せず、製品起動経路が使用します。

libusb は 1.0.30 を固定 URL/SHA-256 から取得し、udev 無効の静的ライブラリとしてリンクします（[PROVENANCE.md](PROVENANCE.md)）。

PR22候補はcold起動のWiden clock gate初期化を`src/product/clock_gate.*`へ追加し、fake/CTestで順序・失敗停止を確認する。実機cold起動/F3成功の証拠ではない。

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
  `DTV_USERLAND_UNIX_PCSC_TORNE_PROFILE=ON` options from the fixed Core548
  input. `torne_pcsc_stage` stages the Unix IFD shared library, Core adapter
  `COPYING`/provenance, Core MIT `LICENSE.dtv-userland-core`, and the reader
  configuration template. It never falls
  back to the PX4 profile; ASICEN and torne profiles remain exclusive.
  `packaging/pcsc/torne.reader.conf.in` is a reader configuration template
  only. `packaging/macos/Info.plist.in` and `packaging/macos/CMakeLists.txt`
  stage a candidate bundle with Core adapter `COPYING`/provenance and Core MIT
  `LICENSE.dtv-userland-core`; the macOS bundle loader is checked in CI, while
  OS registration, real-card use, Linux system-pcscd/other-UID use, and the
  remaining OS integration stay unverified.
- No packaging target edits `/etc`, starts pcscd, installs a system service, or
  replaces a system DLL. The Windows DLL is installed beside the application
  when the product install target is used.

The Core MIT foundation and the GPL-2.0-only PC/SC adapters are separate; the
adapter licenses and provenance are carried with the staged artifacts.
