# torne-userland CLI / daemon framework

これは `torned`・`tornectl`・`torne-ts` の現在の実装、Core 接続、USB列挙、
3 CLI、Core 0/1、DiB8096、CardService、daemon listenerの境界を記す。実機・各OS・
配布物の受入は別で、実装済みと未検証を混同しない。

## 現在の実装範囲

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
- `torned` は設定解析、`DaemonBackend`、`--list-json` の Core JSON encoder、
  USB所有Deviceから`EnclosureFamily`/`EnclosureContext`への接続を持つ。
- 静的 libusb と読み取り専用 `UsbBackend::discover` を追加した。`torned
  --list-json` は CECH-ZD1 (`054c:04b2`) を descriptor と bus/port path から
  列挙する。open/claim/reset/転送/FR30 ロードは行わない。
- 列挙に成功して対象機器が無い場合だけ空一覧を返す。libusb の init や
  device list 取得に失敗した場合は非ゼロで終了する。descriptor や topology が
  得られない対象は scan を incomplete にし、issue と ungrouped observation を
  残して、serial や port を捏造しない。
- `usb/transport` に native transport を追加した。`--device usb:` の bus/port
  で1台を選び、descriptor の interface/endpoint を検証してから interface 0 を
  claim する。kernel driver の暗黙 detach、reset、set_configuration はしない。
  open/claim/control/bulk/close と busy/access/disconnect/timeout/短転送/
  RESULT_UNKNOWN の境界を持つ。`ControlKind` は FR30 契約 §7.1 の副作用分類で、
  PURE_READ/ABS_WRITE だけを1回再送し、未分類は拒否する。
- Android/Termux の借用FD入口を追加した。`torned --fd FD --instance TOKEN` は
  caller所有のdescriptorをtransport内部で`F_DUPFD_CLOEXEC`複製し、
  `libusb_wrap_sys_device`で扱う。transportが閉じるのは複製だけであり、元FDは
  termux-usb等の所有者に残す。CECH-ZD1はUSB function 1個なので、launcher
  `packaging/termux/torne-termux`は`--usb-device`を1回だけ受け、`termux-usb -e`
  のcallback FDを`torned --fd`へ渡す。instanceはUSB pathを列挙できないため必須。
  FD起動は`LIBUSB_OPTION_NO_DEVICE_DISCOVERY`の専用contextを使い、通常のUSB列挙や
  hotplug権限を先行必要条件にしない。先行scanを使わないため、借用FDと無関係な
  先頭機器の位置も公開しない。通常の`--device`と`--list-json`は従来の列挙を維持する。
  2-FD/複数function機器の受渡しはこの入口の対象外である。
- 同期control/bulkと、Core0 TsPump用の非同期bulk submit/cancel/event回収
  （`libusb_alloc_transfer`等）を持つ。TSリング/TS受信はCore0/TsPump経路へ
  接続済み（fake/CTest、実機品質・長時間は未検証）。
- `core1/` に Core 1 ホストruntimeを追加した。S2検証済みを前提に、注入された
  画像metadata/byte列でHOLD→vector→共有data zero/CTRL→code load/全語readback→
  RUN(TRIGGER)→INFO(ABI/build/session/image/data/capability)→heartbeat→NOP ack
  を行う。起動ごとに上位16bit非0・前回異値のsessionを選び、旧echoを拒否する。
  C1-Aの生成headerには依存せず、画像SHA/metadataを明示入力にする。不正画像や
  S2未成立はI/O前に拒否する。製品daemonのHardwareDeviceが追跡画像でこのruntimeを
  起動する。
- `core1/i2c_backend` にC11の`torne_i2c_req/result/backend`と8slot host ringを
  追加した。64bit内部ticket、16bit wire、8slot、session/tag両端、WRITE fast path、
  READ/WRITE_READ完了slot、HALTED/CONTINUE/DISCARD/SKIPPED、NO_COMPLETION/
  CORE_STALLを扱う。S4/clock/GPIO初期化は前提で、backendは自動初期化しない。
  D896/D897/T1Aの実測wireをfakeで固定し、製品daemonのHardwareDeviceは
  Core1 sessionのRingBackendとしてDiB frontendへ接続する。
- `core0/` にCore 0 host runtime、TS engine/USB DMA/EP1設定、非同期Bulk pump、
  192→188 framerを追加した。T1 I²C flush、T2 engine、T3 Core0 load、T4 USB/EP1、
  T5 RUN/INFO/STAT epoch、T6 epoch付きBulk、T7入力開放、P1–P3停止を実装。
  Bulkは64KiB×16の有界キューで、cancel/callback回収と旧epoch破棄を行う。
  framerは192B境界を跨ぐshort/partialを復元し、4B prefixを時刻扱いしない。
  C0-A画像は注入境界で受け、生成headerに依存しない。T8の最初の188B待ちは
  呼出側のpump/framer観測に残る。製品daemonはDevice経由でこのruntimeを接続する。
- `widen/` に read-only の EP0 bridge と登録画像識別を追加した。5語を24bit/4Bで
  読み、PRISTINE / OURS_ACTIVE / FOREIGN_KNOWN / UNKNOWN を内容で分類する。
  FOREIGN_KNOWN は登録レコードの固定長全画像SHA-256・両entry・5パッチ語の全一致、
  OURS_ACTIVE は「今回の画像」として別引数で渡したTWDN画像のヘッダ・全内容・
  entry由来5語の一致を要求する。known側のTWDNは一致してもFOREIGN_KNOWN。
  currentとknownの合計一致が2件以上ならUNKNOWN。登録bankは契約の
  `0xE1000`/`0xE1800`だけで、画像と両entryはその範囲に収める。読取り失敗/
  short/timeoutはUNKNOWNへ変換せず、独立した失敗として返す。識別は導入許可と
  別ゲートで、`introduction_allowed=false` を維持する。PARTIAL_OURSは切替文脈が
  ないため生成しない。
- `fr30/` にCore 1 I²C候補の自作ソースと明示生成targetを追加した。INFO/CTRL/
  STAT、session/heartbeat、NOP ack、8slot ring、byte I²C、HALTED/resumeを
  実装し、ELF/hash/2回生成一致/異常系をCTest `torne_core1_firmware`で確認する。
  通常buildは追跡画像だけを使う。製品起動経路は実機でCore 1 RUN/I²Cへ接続し、
  Linuxの既存T27/T25記録がある。他OS/F4は未検証。任意の`tests/core1_model.py`は
  boot/NOP/byte ringの開発者向けモデル。
- `product/` に`torne-ts --device usb:BUS-PORT`の直接ハードウェア入口を追加した。
  単一TransportでWiden bank0の認識/導入policy、clock/gate、Widen機能検証、Core1
  HOLD/S4/起動、S6 GPIO、DiB init/tune/status、Core0 runtime/非同期Bulk/TS framer
  を順に接続し、188Bを既存`Px4TsOutput`へ出す。`--instance`は従来のCore IPC
  クライアントのまま。fakeでtune→packet出力→packet-count/time停止→stopを確認し、
  既存Linux実測ではT27/T25がexit0。`torned` は同じ `Device` を
  `EnclosureFamily`/`EnclosureContext` へ接続し、`tornectl` と
  `torne-ts --instance` が同じ IPC backend を使う。他OS/F4は未検証。
- `dib/` にDiB8096（DiB8000+DiB0090）のC11 frontendを追加した。GPL-2.0-onlyの
  `torne_drv` `torne_dib.c`と係数表をC11へ適応し、注入した`torne_i2c_backend`
  のsubmit/wait（32B/BE、必要箇所flush）とsleep/clock/log境界で初期化・
  identify・status・GPIO・tuneを呼べる。Core1/GPIO初期化済みを前提とする。
  fakeで4B/3B write、2+2/1+2 read、ゲート順序、GPIO read-modify-write、
  失敗伝播、statusを確認し、製品daemonは同じfrontendを実USB/I²Cへ接続する。
  実機T27/T25の既存記録はあるが、他OS/F4は未検証。
- `fr30/` にCore 0 TS搬送候補の自作ソースと明示生成targetを追加した。INFO/CTRL/
  STAT、session/epoch/heartbeat、NOP/FAULT、内部DMA停止の有界待ち、FIFO/refill、
  packetizer、writer limit更新を実装し、ELF/hash/2回生成一致/異常系をCTest
  `torne_core0_firmware`で確認する。通常buildは追跡画像だけを使う。製品起動経路は
  Core 0 RUN、TSエンジン/USB DMA/EP、実機TSへ接続し、Linuxの既存T27/T25/F3記録が
  ある。他OS/F4は未検証。任意の`tests/core0_model.py`はboot/不正CTRL/DMA停止不能/
  ring wrap/refill/10単位limitの開発者向けモデル。
- `card/uart` にカードUART土台を追加した。既存の単独`usb::Transport`へ注入し、
  24bit/4BのEP0語をレジスタ別の`ControlKind`
  （PURE_READ/ABS_WRITE/CONSUMING_READ/FIFO_WRITE/TRIGGER）で扱う。
  presence、冷resetの10/2/1ms、電源断、FIFO count/RX/TX、SEND一度、
  9600/19200/38400のbaud、注入clockを実装し、short/timeout/USB喪失/unknownの
  非再送・セッション無効化をfakeで確認する。
- `card/service` で製品UARTをCore v1.0.2（source系列548a046）の`CardHardware`/`CardTime`/
  `CardSession`/`CardProtocolSessionAdapter`へ接続した。`EnclosureFamily`の
  card backend/protocolを`CardService`へ渡し、通常daemonのCore listener経路に
  乗る。ATR構文/T=1/APDU/IFSD/WTXはCore既存APIに任せ、CONSUMING_READ/
  FIFO_WRITE/TRIGGERのshort/unknownはUSB_IO等の非再送失敗にする。
  fake UART/clockでpresence/absent/removal、ATR短読み、APDU、旧handle/
  generation、TS進行を確認する。製品daemonは同じCardServiceをUSB所有Deviceへ接続し、
  Linuxの隔離同一UID PC/SC記録がある。Mac/Windows実カード、OS登録、別UIDは未検証。
- `widen/loader` に契約v0.2.2 §6.3のオフライン導入transactionを追加した。
  `IntroductionPolicy` は既定でbank書込み・機能確認とも不許可で、識別結果だけを
  書込み許可に変換しない。PRISTINEはbank0、FOREIGN_KNOWNは現在の反対bankを
  候補にし、OURS_ACTIVEは再ロードしない。画像全語の4B書込み/readback、5語の
  順序書込み/readback、状態変化・短転送・timeout・切断・結果不明での停止、
  TransportのPURE_READ/ABS_WRITE 1回retryを偽装 `UsbIo` で通す。再確認は
  認識時に全画像で一意に特定したrecordと観測5語を保持して同じものを照合し、
  別登録へ選び直さない。Widen後の32bit/最大64Bと旧24bit経路は明示的な段階と
  して扱い、5語確認前はwide APIを呼ばない。成功を認定する機能確認は契約固定の
  64B一括、Core 0/1の別HOLDアドレスと保持値、Core 1予約領域を入口で検査する。
  wide bridgeはhigh 0x00/0x01、終端、4〜64B・4B倍数をI/O前に検査し、0xF0以上の
  予約highを通常read/writeとして送らない。HOLDと64B予約領域試験は
  `allow_functional_check` が真のfakeで確認する。製品HardwareDeviceは追跡画像の
  導入policyを接続するが、cold/ROM互換/rollback/実機書込みの全条件は未検証。

## 検証境界（実装済みと未検証）

- 上のtransport増分にある「同期1回のみ」は初期incrementの記述である。現在の
  製品経路はCore0の非同期Bulk pump、cancel、callback回収、192→188 framerを
  `core0/` と`product/`で接続済みで、古い増分記述を現在の未実装扱いしない。
- Linuxの既存実測では、通常daemonのT27→T25が同一daemonでexit0、自作Core0の
  F3が120/120、隔離同一UID pcscdの標準APIでreader/ATR/T=1/APDUが成功している。
- 初期のdaemon T27はpoll/closeで失敗したが、その後の修正済み記録ではT27→T25
  exit0まで成功している。初期失敗を現在の未実装扱いしない。
- CardServiceの初期HELLOは`UNSUPPORTED`だったが、feature gate修正後のfake IPCと
  隔離PC/SC記録を成功として保持する。初期失敗を未実装のままにしない。
- 未検証: F4の各OS受入、Mac/Windows実カードI/O、OS PC/SC登録/別UID、
  配布archive、FR30 ROM互換/cold/rollbackの全条件、長時間・全チャネル。
- `tornectl`のcard/LNB/serial selectorはCore側で非対応のまま。これはCLIの
  未実装ではなく、製品の公開範囲（カードはPC/SC/WinSCard経路）による。
- PC/SC IFD/WinSCard DLLはstageまで実装済み。OSサービス登録・実アプリでの
  カード利用は未検証で、stage成功をOS統合成功へ読み替えない。

## Core 入力（採用pin）

| 項目 | 値 |
| --- | --- |
| repository | `https://github.com/Khronos31/dtv-userland-core.git` |
| version | `v1.0.2` |
| commit | `cf2aedfaba588827c3dcf024be12b400aa881874`（annotated tag `v1.0.2` object `2fb38e74`。548a046からCMake版1行のみ） |
| 定義場所 | `cmake/TorneCoreDependency.cmake` の1か所 |

cf2aedfa はWindows共通WinSCard fixtureとUnix IFDのtorne profileを含む採用pinであり、最終5製品同pinではない。旧548a046の実装来歴は履歴記述として残す。更新するときは上記のファイルだけを変える。

既定の FetchContent 経路は `GIT_TAG` で上記 commit を取得する。一方
`TORNE_CORE_SOURCE_DIR` は**開発者が指定する未検証の入力**であり、CMake は
checkout の commit や内容を機械検証しない。指定する開発者の責任で、上記
commit（torne profileを含む現行Core）を checkout したツリーを渡す。正式配布物の
入力監査（固定版・ハッシュ・対応ソースの確認）は、この開発者向け近道とは別の
工程で行う。現行実装では SHA 検証コードや強制的な checkout 制約を追加して
いない。

## Core への接続方針（実在 API）

- `tornectl`: `dtv::host_ctl::Profile` + `run_control_cli`。product は
  `identity::Product::torne`。serial selector は `false`。
- `torne-ts`: `dtv::host_ts` の `TsClientProfile` + `parse_px4_ts_product_arguments`
  + `Px4TsRunner::run_wire`。POSIX 出力は `Px4TsFdOutput`、Windows 出力は
  `Px4TsWindowsOutput`、停止は `px4::userland::px4d` のシグナル helper を使う。
- `torned`: `DaemonBackend` が単一 `Device` を `EnclosureFamily` として開き、
  `host_listener_context::EnclosureContext::assemble` へ渡す。`ContextConfig`
  には runtime dir、product (`torne-userland`)、instance、server/list/context
  を設定し、`reserve_namespace` → `publish` → `poll_once` の順で公開する。
  `--device` 省略時は列挙の先頭 enclosure を使い、`--list-json` は claim しない。
- `--list-json` は `UsbBackend::discover` の `wire::DiscoveryScan` を Core
  `wire::discovery_json_size` / `encode_discovery_json` で符号化する。scan の
  product は `torne-userland`、version は製品版。schema は Core の
  `enclosures → devices → receivers / readers` をそのまま使う。
- discovery は serial を持たない製品契約を維持する。`iSerialNumber=0` のため
  `serial` は常に `null`、`serial_unique` も `null`。同じ VID:PID の複数台は
  bus/port path の異なる別 enclosure として返す。root hub 以外の path が
  得られない場合は `invalid_topology` とし、enclosure を作らない。
- `torned` の起動経路は Core listener、所有権、control/stream socket を接続済み。
  実機のopen/claim/tune/TSは測定担当の確認範囲で、Linux CTestのfakeは実機成功を
  意味しない。

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
だけをビルドする。正式配布は固定 llvm-mingw/UCRT で行う予定であり、この確認の
クロスビルド成功を配布物の検証結果には読み替えない。

実機 transport の開発者確認は `-DTORNE_BUILD_LIVE_USB_CHECK=ON` で
`torne_usb_transport_live_check` をビルドし、`--device usb:BUS-PORT` を渡す。
これは CTest ではなく、open/claim/標準descriptor読み/二重claim/再openだけを行う。

## ライセンスと出典

製品コードは GPL-2.0-only。Core の portable 部分は MIT、GPL adapter は
GPL-2.0-only で、Core 側の PROVENANCE に従う。libusb 1.0.30 は
LGPL-2.1-or-later で、静的リンクの義務と出典は `PROVENANCE.md` に記録する。
