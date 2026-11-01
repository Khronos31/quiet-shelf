#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# CI entry for the existing macOS Release job. Uses the same product-owned
# toolchain and release helper as docs/release-foundation.md; no upload.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd -P)
core_file=$root/cmake/TorneCoreDependency.cmake
libusb_file=$root/cmake/TorneLibusb.cmake
config=$root/scripts/release/macos-toolchain.json
pcsc_include=/opt/homebrew/opt/pcsc-lite/include/PCSC

test -r "$pcsc_include/ifdhandler.h" || {
    printf '%s\n' "missing build-only pcsc-lite header: $pcsc_include/ifdhandler.h" >&2
    exit 1
}
test -r "$config" || {
    printf '%s\n' "missing macOS toolchain config: $config" >&2
    exit 1
}

version=$(python3 - "$root/CMakeLists.txt" <<'PY'
import re
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r'project\(torne_userland VERSION ([0-9]+\.[0-9]+\.[0-9]+)', text)
if match is None:
    raise SystemExit("cannot read project version")
print(match.group(1))
PY
)
core_commit=$(python3 - "$core_file" <<'PY'
import re
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r'set\(TORNE_CORE_COMMIT "([0-9a-f]{40})"\)', text)
if match is None:
    raise SystemExit("cannot read Core commit")
print(match.group(1))
PY
)
libusb_input=$(python3 - "$libusb_file" <<'PY'
import re
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding="utf-8")
version = re.search(r'set\(TORNE_LIBUSB_VERSION "([^"]+)"\)', text)
url = re.search(r'"https://[^"]+/libusb-[^"]+\.tar\.bz2"', text)
digest = re.search(r'set\(TORNE_LIBUSB_SHA256 "([0-9a-f]{64})"\)', text)
if version is None or url is None or digest is None:
    raise SystemExit("cannot read libusb input")
print(version.group(1))
print(url.group(0).strip('"').replace("${TORNE_LIBUSB_VERSION}", version.group(1)))
print(digest.group(1))
PY
)
libusb_version=$(printf '%s\n' "$libusb_input" | sed -n '1p')
libusb_url=$(printf '%s\n' "$libusb_input" | sed -n '2p')
libusb_sha=$(printf '%s\n' "$libusb_input" | sed -n '3p')

work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/torne-macos-release.XXXXXX")
core=$work/core
git init -q "$core"
git -C "$core" fetch --depth 1 https://github.com/Khronos31/dtv-userland-core.git "$core_commit"
git -C "$core" checkout --detach FETCH_HEAD
test "$(git -C "$core" rev-parse HEAD)" = "$core_commit"

libusb=$work/libusb-$libusb_version.tar.bz2
curl -fsSL --retry 3 "$libusb_url" -o "$libusb"
actual=$(shasum -a 256 "$libusb" | awk '{print $1}')
test "$actual" = "$libusb_sha" || {
    printf '%s\n' "libusb checksum mismatch: $actual" >&2
    exit 1
}

output=$work/output
"$root/scripts/release/build-candidate.sh" \
    --output "$output" \
    --core-source "$core" \
    --libusb-source-archive "$libusb" \
    --darwin-arm64 \
    --toolchain-config "$config"

test -f "$output/torne-userland-$version-darwin-arm64.tar.gz"
test -f "$output/SHA256SUMS"
shasum -a 256 "$output"/*.tar.gz "$output/SHA256SUMS"
printf '%s\n' "macOS release helper completed: $output"
