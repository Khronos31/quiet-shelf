<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# FR30 ファームウェア

CECH-ZD1J の内蔵ブリッジチップ（MB86H47A）に搭載された FR30 コア（Widen / Core 0 / Core 1）向けのファームウェアアセット、ソースコード、リンカスクリプト、生成スクリプト群である。

## 1. 構成と役割

本ディレクトリには、自作 FR30 ファームウェアの候補実装およびその生成・管理に必要なファイルを配置している。

- `torne-fr30-v1.bin`: 配布用の結合ファームウェアバイナリ（`fr30/generated/torne-fr30-v1.bin` の配置コピー）
- `NOTICE`: 先行実装 coax プロジェクトへの帰属表示
- `fr30/src/`: FR30 アセンブリソースコード（`widen.s`, `core0.s`, `core1.s`）
- `fr30/ld/`: リンカスクリプト（`widen-bank0.ld`, `widen-bank1.ld`, `core0.ld`, `core1.ld`）
- `fr30/generated/`: 生成された ELF、raw バイナリ、ヘッダ、マニフェスト、結合アセット
- `scripts/fr30/`: ファームウェアアセット組立用 Python スクリプト（`generate_firmware_asset.py`）
- `cmake/`: 各コア画像生成およびアセット組立用の個別 CMake スクリプト群

## 2. 配布用バイナリと構成アセット

配布用の結合バイナリ `firmware/torne-fr30-v1.bin` は、3 つの raw バイナリ画像を連結したもので、合計 3,428 バイトである。

- **ファイル名**: `torne-fr30-v1.bin`
- **サイズ**: 3,428 バイト
- **SHA-256**: `8a7a5cac3cc6b6309020f3db0dc086dcec387e5fa2af1185b1a4b6c8455fd506`
- **生成物との関係**: `firmware/fr30/generated/torne-fr30-v1.bin` とバイト単位で完全一致（配置コピー）

### 結合アセットの内訳

| アセット名 | オフセット | サイズ | SHA-256 | 技術上の役割 |
|---|---:|---:|---|---|
| **Widen bank0** | 0 | 172 bytes | `8fd035bce738a91fa78944423b2e7383e3927faec079c9f82bd51baf422fa53d` | Widen read/write ハンドラ候補 |
| **Core 0** | 172 | 1,332 bytes | `b59b82dd81f3908cf95bb290d5bdd229b12b5c9de13148bd44be1e5449522274` | TS 搬送ハンドラ候補 |
| **Core 1** | 1,504 | 1,924 bytes | `0e02c9468936bb4411323939baca42e1dc85559e29da632cc186998e6a0ea202` | I2C マスタ制御候補 |

※ `fr30/generated/` 配下には Widen bank1 用のバイナリ（`widen-bank1.bin`）および ELF も生成物として保持されているが、配布用結合アセット `torne-fr30-v1.bin` に連結されているのは Widen bank0 である。各アセットの詳細なオフセット、エントリポイント、ビルド ID 等は `fr30/generated/torne-fr30-v1-manifest.json` に記録されている。

### 実機未検証の候補実装

各マニフェスト（`*-manifest.json`）に記録された状態（status）は `candidate-unverified-on-device` である。これらはホスト側からのロード、HOLD/RUN 制御、TS エンジン、USB DMA、共有メモリ（V-MEM）、I2C 通信などの実機ハードウェア動作を確認したものではなく、候補実装にとどまる。

### マニフェストのハッシュメタデータに関する留意事項

各マニフェスト（`*-manifest.json`）に記録されている `source_sha256` および `linker_sha256` は、移設元のライセンス表示編集前（`GPL-2.0-only` 当時）のハッシュ値である。移設時にアセンブリソース（`.s`）およびリンカスクリプト（`.ld`）の SPDX 表示を `GPL-2.0-or-later` へ更新したため、現在のソースおよびリンカスクリプトの実ファイルハッシュとは異なっている。

現行のコミット済みマニフェストは、意図してこの元記録のまま保持している。将来的に再生成処理を実行した場合は、現行ファイルからハッシュ値が再計算されてメタデータが更新され得るが、移設時に配置されたバイナリ画像（`.bin` および `.elf`）は元コミットとバイト単位で完全に同一である。ライセンス表示更新に伴うメタデータハッシュの不一致や将来の更新可能性を、画像バイナリ自体の変更や再生成の実施・成功と混同してはならない。

## 3. ライセンスと帰属表示

### 自作 FR30 ファームウェアのライセンス
自作の FR30 ファームウェア部分（アセンブリソース、リンカスクリプト、CMake、Python スクリプト、生成ヘッダ・マニフェスト等）は、著作権者の明示的な判断に基づき、`GPL-2.0-or-later` を適用している。

### 先行実装（coax）の帰属とライセンス選択
CECH-ZD1J の内蔵ブリッジチップ（MB86H47A）のプロトコルおよびハードウェア値の解釈にあたっては、先行リバースエンジニアリングプロジェクトである coax（Copyright 2026 Till0196）の知見を参照している。coax が提示するデュアルライセンス（`Apache-2.0 OR GPL-2.0-or-later`）のうち、本プロジェクトは `GPL-2.0-or-later` を選択しており、原著作者の著作権表示および帰属表示を `firmware/NOTICE` として保持している。

### 第三者コードおよび公式ファームウェアに関する申告
著作権者は Sony 公式のファームウェアバイナリを一切閲覧・確認していないことを明示している。本項目はこの申告を客観的記録として記載するものであり、第三者由来の権利侵害が一切存在しないという法的な非侵害保証を行うものではない。

## 4. ビルド・生成方法

通常の製品ビルドではリポジトリにコミット済みのバイナリアセットのみを使用するため、FR30 のクロス開発ツールは不要である。ファームウェアを再生成する場合のみ、以下のツールとスクリプトを使用する。

### 必要ツール
- **GNU binutils 2.47** (`fr30-elf` ターゲット: `fr30-elf-as`, `fr30-elf-ld`, `fr30-elf-objcopy` 等)
- **Python 3**
- **CMake 3.16 以上**

binutils 2.47 の入手元アーカイブ URL、署名・ハッシュ検証情報、および過去の検証時のビルド条件記録については、[fr30/README.md](fr30/README.md) を参照のこと。

### 生成手順
各コア画像の生成は、アセンブラ（`as --fatal-warnings`）、リンカ（`ld -m elf32fr30` / `-nostdlib`）、raw バイナリ変換（`objcopy -O binary`）を経て行われる。

1. **各コア画像の生成**:
   以下の個別 CMake スクリプトに対し、binutils のパスと出力先を指定して個別実行する。
   - `cmake/fr30_widen.cmake` (引数: `-DTORNE_FR30_BINUTILS_DIR=... -DTORNE_WIDEN_OUTPUT_DIR=...`)
   - `cmake/fr30_core0.cmake` (引数: `-DTORNE_FR30_BINUTILS_DIR=... -DTORNE_CORE0_OUTPUT_DIR=...`)
   - `cmake/fr30_core1.cmake` (引数: `-DTORNE_FR30_BINUTILS_DIR=... -DTORNE_CORE1_OUTPUT_DIR=...`)
2. **結合アセットの組み立て**:
   `cmake/fr30_asset_generate.cmake` を実行する。内部で `scripts/fr30/generate_firmware_asset.py` が呼び出され、`fr30/generated/` 配下の各画像から `torne-fr30-v1.bin`、マニフェスト、メタデータヘッダが組み立てられる。
3. **配布用バイナリの手動同期**:
   アセット組立スクリプトの出力先は `firmware/fr30/generated/torne-fr30-v1.bin` である。配布用バイナリ `firmware/torne-fr30-v1.bin` への自動コピー処理は CMake スクリプトに含まれていないため、再生成後は手動でコピーして同期する必要がある。

### 注意事項
本リポジトリへの移設作業においては、配置されたバイナリが移設元コミットのファイルと SHA-256 およびバイト単位で一致することを確認したのみであり、**実機環境での再生成やビルドは実施していない**（生成環境は Windows 前提とされている）。バイナリの一致確認は、現環境での再生成の成功を意味するものではない。
