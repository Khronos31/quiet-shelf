<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# dtv11-userland-core

dtv11 userland 各製品の共通コア。`libdtv-userland.a` をビルドし、生成コマンドラインのエントリポイントと製品向け CMake 関数を提供する。

fixture やビルド手順は [製品への組み込み](docs/product-integration.md) を、公開スナップショットの手順は [quiet-shelf CI](docs/quiet-shelf-ci.md) を参照すること。この初期段階の実装はメタデータ要求のみに対応しており、ハードウェアやデーモンの操作は未実装である。

本プロジェクトは GPL-2.0-or-later の下でライセンスされている。[LICENSE](LICENSE) を参照すること。
