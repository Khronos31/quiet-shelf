<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# quiet-shelf CI snapshot

This repository is private. The public quiet-shelf CI must use ordinary files and
must not fetch this repository as a submodule.

## Prepare a public snapshot

From a local clone of this repository, select the reviewed Core commit SHA and export
its tracked files into a folder in a quiet-shelf working branch:

```sh
CORE_SHA='reviewed-commit-sha'
mkdir -p dtv11-core
git -C /path/to/local/dtv11-core archive "$CORE_SHA" | tar -x -C dtv11-core
```

The `dtv11-core` folder contains the Core and its product fixture as ordinary files.
Do not copy `build/` or add a gitlink. Copy
`dtv11-core/.github/workflows/quiet-shelf-ci.yml` into the quiet-shelf repository's
`.github/workflows/core-ci.yml` so GitHub Actions runs the included fixture from its
subdirectory. The private Core `.github/workflows/ci.yml` remains for Core PRs.

```sh
cp dtv11-core/.github/workflows/quiet-shelf-ci.yml \
  /path/to/quiet-shelf/.github/workflows/core-ci.yml
```

## Pull request

Commit the snapshot and workflow on a branch of `Khronos31/quiet-shelf`, then open a
Japanese-language PR to `main`:

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

This procedure only describes the later public validation step. This fix task does
not publish the snapshot or create the quiet-shelf PR.
