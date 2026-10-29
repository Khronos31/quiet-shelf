# torne-userland

Sony CECH-ZD1 J（USB \`054c:04b2\`）用の、カーネルドライバ不要の ISDB-T USB チューナードライバ。**開発中／実機での独立実装未検証**。プロジェクトはGPL-2.0-only。

## 現在の実装範囲

- \`dtv-userland-core v0.1.0\`の**現在確認済みコミット** \`32731433803f01903b4f198ddd94647c4d222ac0\` をCMake FetchContentで固定。タグの打ち直しを追跡しない。
- GPL-2.0-onlyの[mihalinn/torne_drv](https://github.com/mihalinn/torne_drv)のC製ハードウェア中核（ブリッジ／DiB8096／FR30／TS変換）を、著作権表示・元LICENSE・NOTICEと共に取り込み。
- 共通Coreの選局リゾルバ、USBパスセレクタ、Discovery JSONを使用。独自チャンネル表・JSONシリアライザは持たない。
- Cハードウェア層とCoreを分離し、\`torne_ops\`を介してlibusbを接続。
- \`torne-probe\`は**一回限りの実験用CLI**。まだ製品用\`torned\`／\`tornectl\`のdaemon・IPC実装ではない。

## ビルド

CMake 3.21+、C/C++17コンパイラ、Git。USBツールにはlibusb-1.0の開発用ヘッダーが必要。

\`\`\`sh
cmake -S . -B build -DTORNE_BUILD_TESTS=ON -DTORNE_BUILD_LIBUSB=ON
cmake --build build
ctest --test-dir build --output-on-failure
\`\`\`

ネットワークなしでのCoreのビルドを試す場合は、事前に同じコミットのCoreソースを用意し、\`-DFETCHCONTENT_SOURCE_DIR_DTV_USERLAND_CORE=/path/to/core\`を指定する。バージョン0.1.0へのピンは\`CMakeLists.txt\`内のGit SHAを確認。

### 実験用コマンド

\`\`\`sh
./build/torne-probe --version
./build/torne-probe --resolve T27
./build/torne-probe --list-json
\`\`\`

\`--list-json\`はlibusb descriptorの観測のみを実行し、USBデバイスのclaim、kernel driverのdetach、初期化、選局をしない。CECH-ZD1 Jに一意なUSBシリアルが無い個体を確認済みのため、製造番号の合成やシリアルによる一意選択をしない。

明示的な実機検証時のみ、他プロセスの占有や試験状況を確認してから次を使う。

\`\`\`sh
./build/torne-probe --tune T27 --device usb:1-2 --seconds 10 > capture.ts
\`\`\`

これはチップ制御・RF選局・USBデータ取得を伴う。**このリポジトリ作業で実機試験はしていない**。実行は実測担当・ユーザーの承認後に限る。

## 今後の作業

- 共通Core v0.1.0の受信機所有権・capture・control・statusの契約と、製品側\`torned/tornectl\`を接続する。
- 長時間USB受信は同期読みから複数in-flight転送とキャンセル管理へ拡張する。
- 既存C版のFR30／DiB8096受信をT27で既存coax実測と照合する。
- カードUART／T=1リーダーとコアカード所有権は独立モジュールとして後付け。
- denpaの高速化アルゴリズムは**コードをコピーせず**独立実装する。

[設計上の境界](docs/ARCHITECTURE.md) · [出典・著作権](PROVENANCE.md)
