<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# quiet-shelf CI スナップショット

本リポジトリは private である。公開の quiet-shelf CI では通常ファイルを使用する必要があり、本リポジトリを submodule として取得してはならない。

## 公開スナップショットの準備

本リポジトリのローカルクローンから、レビュー済みの Core コミット SHA を選択し、追跡対象ファイルを quiet-shelf の作業ブランチ内フォルダへ展開する。

```sh
CORE_SHA='reviewed-commit-sha'
mkdir -p dtv11-core
git -C /path/to/local/dtv11-core archive "$CORE_SHA" | tar -x -C dtv11-core
```

`dtv11-core` フォルダには、Core と製品 fixture が通常ファイルとして配置される。`build/` をコピーしたり、gitlink を追加したりしてはならない。`dtv11-core/.github/workflows/quiet-shelf-ci.yml` を quiet-shelf リポジトリの `.github/workflows/core-ci.yml` にコピーすることで、GitHub Actions がサブディレクトリから同梱の fixture を実行できるようにする。private な Core の `.github/workflows/ci.yml` は、Core の PR 用としてそのまま残す。

```sh
cp dtv11-core/.github/workflows/quiet-shelf-ci.yml \
  /path/to/quiet-shelf/.github/workflows/core-ci.yml
```

## プルリクエスト

スナップショットとワークフローを `Khronos31/quiet-shelf` のブランチにコミットし、`main` 宛てに日本語の PR を作成する。

```sh
git -C /path/to/quiet-shelf switch main
git -C /path/to/quiet-shelf pull --ff-only origin main
git -C /path/to/quiet-shelf switch -c dtv11-core-ci
git -C /path/to/quiet-shelf add dtv11-core .github/workflows/core-ci.yml
git -C /path/to/quiet-shelf commit -m 'dtv11 CoreのLinux/MSVC CIを追加'
git -C /path/to/quiet-shelf push -u origin dtv11-core-ci
gh pr create -R Khronos31/quiet-shelf --base main --head dtv11-core-ci \
  --title 'dtv11 CoreのLinux/MSVC CIを追加' \
  --body 'Coreを通常ファイルとして同梱し、LinuxとMSVCでfixtureのbuildとCLI契約表を確認します。'
```

この手順は後日の公開検証ステップを説明するものである。本修正タスクではスナップショットの公開や quiet-shelf PR の作成は行わない。
