# torne-userland CLI / daemon framework

これは `torned`・`tornectl`・`torne-ts` の入口、Core 接続、静的 libusb による
非侵襲 USB 列挙までの増分である。完成したデーモンでも、FR30/DiB8096/カードが
動く実装でもない。

## この増分の範囲

実装済み:

- CMake だけで3実行ファイル（`torned`, `tornectl`, `torne-ts`）を生成する。
  `torne-probe` は生成せず、配布対象にも含めない。
- ISO C11 / ISO C++17、言語拡張 OFF。
- `tornectl` は Core `host_ctl::run_control_cli` と `Profile` をそのまま使い、
  product slug は `torne-userland`。card/LNB/serial selector は非対応のまま
  Core がその旨を返す。
- `--instance` は3コマンドとも Core の `identity::canonicalize_instance` と同じ
  規則を使う。大文字小文字は保存し、予約名（`com1` 等）・先頭/終端の不正な
  記号は同じように拒否する。
- `torne-ts` は Core `host_ts` の引数解析・usage・`TsClientProfile`・
  `Px4TsRunner::run_wire` を使う。製品側で文字列や選択規則を複製しない。
  ISDB-T 以外は明示エラーにする。
- `torned` は設定解析、`Backend` 境界、`--list-json` の Core JSON encoder 接続、
  未実装時の非ゼロ終了を持つ。
- 静的 libusb と読み取り専用 `UsbBackend::discover` を追加した。`torned
  --list-json` は CECH-ZD1 (`054c:04b2`) を descriptor と bus/port path から
  列挙する。open/claim/reset/転送/FR30 ロードは行わない。
- 列挙に成功して対象機器が無い場合だけ空一覧を返す。libusb の init や
  device list 取得に失敗した場合は非ゼロで終了する。descriptor や topology が
  得られない対象は scan を incomplete にし、issue と ungrouped observation を
  残して、serial や port を捏造しない。

未実装（後続の増分）:

- USB control/bulk transport、MB86H47A bridge、DiB8096 frontend、カード UART、FR30。
- `UsbBackend::open_family`。現状は `UNSUPPORTED` を返し、claim しない。
- Core `host_listener_context::EnclosureContext` の assemble、authority/worker
  接続、control/stream socket の publish、`poll_once` ループ。
- 実機なしで検証できる範囲を超える、PC/SC IFD、WinSCard DLL、配布物。

## Core 入力（暫定）

| 項目 | 値 |
| --- | --- |
| repository | `https://github.com/Khronos31/dtv-userland-core.git` |
| commit | `301c4593fc34702729750477d93f9d9e0051b0cb`（v1.0.1 実体） |
| 定義場所 | `cmake/TorneCoreDependency.cmake` の1か所 |

v1.0.1 は現時点の再現可能な参照であり、製品の最終 pin ではない。Core 側の
整理後に5製品と同じ新 Core へ揃える。更新するときは上記のファイルだけを
変える。

既定の FetchContent 経路は `GIT_TAG` で上記 commit を取得する。一方
`TORNE_CORE_SOURCE_DIR` は**開発者が指定する未検証の入力**であり、CMake は
checkout の commit や内容を機械検証しない。指定する開発者の責任で、上記
commit を checkout したツリーを渡す。正式配布物の入力監査（固定版・ハッシュ・
対応ソースの確認）は、この開発者向け近道とは別の工程で行う。今回の増分では
SHA 検証コードや強制的な checkout 制約を追加していない。

## Core への接続方針（実在 API）

- `tornectl`: `dtv::host_ctl::Profile` + `run_control_cli`。product は
  `identity::Product::torne`。serial selector は `false`。
- `torne-ts`: `dtv::host_ts` の `TsClientProfile` + `parse_px4_ts_product_arguments`
  + `Px4TsRunner::run_wire`。POSIX 出力は `Px4TsFdOutput`、Windows 出力は
  `Px4TsWindowsOutput`、停止は `px4::userland::px4d` のシグナル helper を使う。
- `torned`: `Backend` が返す `dtv::userland::host_runtime::EnclosureFamily`
  （tuner/card backend、card protocol、time、stream control）を、将来
  `host_listener_context::EnclosureContext::assemble` へ渡す。`ContextConfig`
  には runtime dir、product (`torne-userland`)、instance、server/list/context
  を設定し、`reserve_namespace` → `publish` → `poll_once` の順で公開する。
  `UsbBackend` の `open_family` はまだ `UNSUPPORTED` を返し、claim しない。
- `--list-json` は `UsbBackend::discover` の `wire::DiscoveryScan` を Core
  `wire::discovery_json_size` / `encode_discovery_json` で符号化する。scan の
  product は `torne-userland`、version は製品版。schema は Core の
  `enclosures → devices → receivers / readers` をそのまま使う。
- discovery は serial を持たない製品契約を維持する。`iSerialNumber=0` のため
  `serial` は常に `null`、`serial_unique` も `null`。同じ VID:PID の複数台は
  bus/port path の異なる別 enclosure として返す。root hub 以外の path が
  得られない場合は `invalid_topology` とし、enclosure を作らない。
- 今の `torned` 起動経路は `open_family` が `UNSUPPORTED` のため、列挙だけが
  可能である。listener/所有権/IPC は次の増分。

`Backend` の fake はテスト `tests/daemon_tests.cpp` にだけ置く。製品ビルドには
含めない。

## ビルドと検証

```sh
cmake -S . -B build -G Ninja -DTORNE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

libusb は `cmake/TorneLibusb.cmake` の固定 URL/SHA-256 から取得し、libusb
自身の Autotools で `--disable-udev --disable-shared --enable-static` を付けて
ビルドする。製品用の別 Makefile は持たない。初回ビルドではネットワーク取得が
発生する。CMake が選んだ cross target、sysroot、compiler 引数は libusb の
configure へ引き継ぎ、Android なども対象アーキテクチャでビルドする。Darwin では
libusb 上流の `Libs.private` に合わせて Objective-C runtime と OS 標準 framework
（IOKit/CoreFoundation/Security）を最終リンクへ伝播する。

ネットワークを使わない場合は、固定 commit を checkout した Core を
`-DTORNE_CORE_SOURCE_DIR=/path/to/dtv-userland-core` で渡す。この指定は
SHA 検証されない開発者向け入力であり、正式配布の入力監査とは別である。

Windows クロス確認は同じ Core 入力を固定し、`TORNE_BUILD_TESTS=OFF` で3 target
だけをビルドする。正式配布は固定 llvm-mingw/UCRT で行う予定であり、この増分の
クロスビルド成功を配布物の検証結果には読み替えない。

## ライセンスと出典

製品コードは GPL-2.0-only。Core の portable 部分は MIT、GPL adapter は
GPL-2.0-only で、Core 側の PROVENANCE に従う。libusb 1.0.30 は
LGPL-2.1-or-later で、静的リンクの義務と出典は `PROVENANCE.md` に記録する。
