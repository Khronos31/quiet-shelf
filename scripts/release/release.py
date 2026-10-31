#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the REL-01 source and Linux x86_64 glibc release candidate."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PRODUCT = "torne-userland"
PLATFORM = "linux-glibc-x86_64"
BINARIES = ("torned", "tornectl", "torne-ts")
IFD_SYMBOLS = (
    "IFDHCreateChannel",
    "IFDHCreateChannelByName",
    "IFDHCloseChannel",
    "IFDHGetCapabilities",
    "IFDHSetCapabilities",
    "IFDHSetProtocolParameters",
    "IFDHPowerICC",
    "IFDHTransmitToICC",
    "IFDHControl",
    "IFDHICCPresence",
)
ALLOWED_NEEDED = {
    "libc.so.6",
    "libm.so.6",
    "libpthread.so.0",
    "librt.so.1",
    "libdl.so.2",
    "ld-linux-x86-64.so.2",
}
FORBIDDEN_NEEDED = (
    "libusb",
    "libpcsclite",
    "libudev",
    "libstdc++",
    "libgcc_s",
    "libc++",
)
# Standard glibc loaders for the only binary platform in this increment.
# The same loader is commonly reachable through these distro-standard aliases.
ALLOWED_INTERPRETERS = frozenset({
    "/lib64/ld-linux-x86-64.so.2",
    "/lib/ld-linux-x86-64.so.2",
    "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
    "/usr/lib64/ld-linux-x86-64.so.2",
    "/usr/lib/ld-linux-x86-64.so.2",
    "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
})


def fail(message: str) -> None:
    print(f"release: {message}", file=sys.stderr)
    raise SystemExit(1)


def run(args: list[str], *, cwd: Path | None = None,
        capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, check=True, text=True, capture_output=capture)


def output(args: list[str], *, cwd: Path | None = None) -> str:
    return run(args, cwd=cwd, capture=True).stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(torne_userland VERSION ([0-9]+\.[0-9]+\.[0-9]+)", text)
    if match is None:
        fail("cannot read project version")
    return match.group(1)


def read_core_commit() -> str:
    text = (ROOT / "cmake/TorneCoreDependency.cmake").read_text(encoding="utf-8")
    match = re.search(r'set\(TORNE_CORE_COMMIT "([0-9a-f]{40})"\)', text)
    if match is None:
        fail("cannot read Core commit")
    return match.group(1)


def read_libusb() -> tuple[str, str, str]:
    text = (ROOT / "cmake/TorneLibusb.cmake").read_text(encoding="utf-8")
    version = re.search(r'set\(TORNE_LIBUSB_VERSION "([^"]+)"\)', text)
    url = re.search(r'"https://[^"]+/libusb-[^"]+\.tar\.bz2"', text)
    digest = re.search(r'set\(TORNE_LIBUSB_SHA256 "([0-9a-f]{64})"\)', text)
    if version is None or url is None or digest is None:
        fail("cannot read libusb input")
    resolved_url = url.group(0).strip('"').replace("${TORNE_LIBUSB_VERSION}", version.group(1))
    return version.group(1), resolved_url, digest.group(1)


def require_clean_source() -> str:
    status = output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT)
    if status:
        fail("tracked worktree is not clean; commit before building a release candidate")
    return output(["git", "rev-parse", "HEAD"], cwd=ROOT)


def checkout_identity() -> dict[str, str]:
    repository = output(["git", "remote", "get-url", "origin"], cwd=ROOT)
    repository = re.sub(r"//[^/@]+@", "//", repository)
    if repository.endswith(".git"):
        repository = repository[:-4]
    return {
        "repository": repository,
        "commit": output(["git", "rev-parse", "HEAD"], cwd=ROOT),
        "tree": output(["git", "rev-parse", "HEAD^{tree}"], cwd=ROOT),
    }


def read_source_map() -> tuple[dict[str, str] | None, str | None]:
    path = ROOT / "SOURCE-MAP.json"
    tracked = subprocess.run(
        ["git", "cat-file", "-e", "HEAD:SOURCE-MAP.json"],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not tracked:
        if path.exists() or path.is_symlink():
            fail("SOURCE-MAP.json must be tracked at the checkout commit before manufacture")
        return None, None
    try:
        raw = subprocess.check_output(["git", "show", "HEAD:SOURCE-MAP.json"], cwd=ROOT)
        data = json.loads(raw.decode("utf-8"))
    except (OSError, subprocess.CalledProcessError, UnicodeDecodeError,
            json.JSONDecodeError) as error:
        fail(f"invalid SOURCE-MAP.json: {error}")
    product = data.get("product") if isinstance(data, dict) else None
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(product, dict):
        fail("SOURCE-MAP.json must contain schema 1 and product")
    for key in ("repository", "commit", "tree"):
        if not isinstance(product.get(key), str) or not product[key]:
            fail(f"SOURCE-MAP.json product.{key} is missing")
    if not re.fullmatch(r"[0-9a-f]{40}", product["commit"]):
        fail("SOURCE-MAP.json product.commit must be a 40-character SHA")
    if not re.fullmatch(r"[0-9a-f]{40}", product["tree"]):
        fail("SOURCE-MAP.json product.tree must be a 40-character SHA")
    return {"repository": product["repository"], "commit": product["commit"],
            "tree": product["tree"]}, hashlib.sha256(raw).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def safe_extract(archive: tarfile.TarFile, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for member in archive.getmembers():
        name = member.name
        if name.startswith("/") or "\\" in name:
            fail(f"unsafe archive path: {name}")
        parts = Path(name).parts
        if ".." in parts or member.issym() or member.islnk():
            fail(f"unsupported archive member: {name}")
        archive.extract(member, destination)


def deterministic_tar(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as archive:
                for path in sorted(p for p in source.rglob("*") if p.is_file()):
                    if path.is_symlink():
                        fail(f"symlink in archive source: {path}")
                    relative = path.relative_to(source.parent).as_posix()
                    info = tarfile.TarInfo(relative)
                    info.size = path.stat().st_size
                    info.mode = path.stat().st_mode & 0o7777
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    with path.open("rb") as stream:
                        archive.addfile(info, stream)


def git_archive(repository: Path, commit: str, destination: Path) -> None:
    with destination.open("wb") as stream:
        subprocess.run(["git", "archive", commit], cwd=repository, check=True,
                       stdout=stream)


def download_libusb(url: str, expected: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url) as response, destination.open("wb") as stream:
        shutil.copyfileobj(response, stream)
    if sha256(destination) != expected:
        fail("downloaded libusb archive checksum mismatch")


def extract_libusb_copying(archive_path: Path, destination: Path) -> None:
    with tarfile.open(archive_path, "r:bz2") as archive:
        try:
            member = archive.getmember("libusb-1.0.30/COPYING")
        except KeyError:
            fail("libusb archive has no COPYING")
        stream = archive.extractfile(member)
        if stream is None:
            fail("cannot extract libusb COPYING")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(stream.read())


def source_archive(version: str, product_source: dict[str, str],
                   checkout: dict[str, str], source_map_sha256: str | None,
                   core_commit: str, core_source: Path, libusb_archive: Path,
                   libusb_version: str, libusb_url: str, libusb_sha256: str,
                   work: Path, output_dir: Path) -> tuple[Path, Path]:
    source_root = work / f"{PRODUCT}-{version}-source"
    source_root.mkdir(parents=True)
    product_tar = work / "product.tar"
    git_archive(ROOT, checkout["commit"], product_tar)
    with tarfile.open(product_tar) as archive:
        safe_extract(archive, source_root)

    core_root = source_root / "third_party" / "dtv-userland-core"
    core_root.mkdir(parents=True)
    core_tar = work / "core.tar"
    git_archive(core_source, core_commit, core_tar)
    with tarfile.open(core_tar) as archive:
        safe_extract(archive, core_root)

    libusb_root = source_root / "third_party" / "libusb"
    libusb_root.mkdir(parents=True)
    libusb_name = f"libusb-{libusb_version}.tar.bz2"
    shutil.copy2(libusb_archive, libusb_root / libusb_name)
    extract_libusb_copying(libusb_archive, libusb_root / "COPYING")
    (libusb_root / "SOURCE.txt").write_text(
        f"version={libusb_version}\nurl={libusb_url}\nsha256={libusb_sha256}\n",
        encoding="ascii",
    )

    source_map = None
    if source_map_sha256 is not None:
        source_map = {"path": "SOURCE-MAP.json", "sha256": source_map_sha256}
    source_metadata = {
        "schema": 1,
        "product": PRODUCT,
        "version": version,
        "product_source": product_source,
        "checkout": checkout,
        "source_map": source_map,
        "repository": product_source["repository"],
        "source_commit": product_source["commit"],
        "source_tree": product_source["tree"],
        "core_repository": "Khronos31/dtv-userland-core",
        "core_commit": core_commit,
        "libusb_version": libusb_version,
        "libusb_url": libusb_url,
        "libusb_sha256": libusb_sha256,
        "rebuild": {
            "core_source": "third_party/dtv-userland-core",
            "libusb_source_archive": f"third_party/libusb/{libusb_name}",
        },
    }
    write_json(source_root / "SOURCE-METADATA.json", source_metadata)

    rebuild_lines = [
        f"# Rebuild {PRODUCT} {version}",
        "",
        "The fixed Core and libusb sources are bundled in this archive.",
        "A platform toolchain and the system PC/SC development header are still required.",
        "",
    ]
    if source_map_sha256 is not None:
        rebuild_lines += [
            f"This source archive was made from a checkout of {checkout['repository']} at {checkout['commit']}.",
            f"The product source mapping is {product_source['repository']} at {product_source['commit']} "
            f"(tree {product_source['tree']}); see SOURCE-METADATA.json and SOURCE-MAP.json.",
            "",
        ]
    rebuild_lines += [
        "```sh",
        "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \\",
        "  -DTORNE_BUILD_TESTS=OFF -DTORNE_BUILD_PCSC_IFD=ON \\",
        '  -DTORNE_CORE_SOURCE_DIR="$PWD/third_party/dtv-userland-core" \\',
        f'  -DTORNE_LIBUSB_SOURCE_ARCHIVE="$PWD/third_party/libusb/{libusb_name}" \\',
        '  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \\',
        '  -DCMAKE_SHARED_LINKER_FLAGS="-static-libstdc++ -static-libgcc"',
        "cmake --build build --target torne_torned torne_tornectl torne_torne_ts torne_pcsc_stage",
        "```",
    ]
    (source_root / "REBUILD.md").write_text("\n".join(rebuild_lines) + "\n", encoding="utf-8")

    archive_path = output_dir / f"{PRODUCT}-{version}-source.tar.gz"
    deterministic_tar(source_root, archive_path)
    return archive_path, source_root


def build_from_source(version: str, source_archive_path: Path, work: Path,
                      jobs: int) -> Path:
    extract = work / "source-build"
    extract.mkdir()
    with tarfile.open(source_archive_path) as archive:
        safe_extract(archive, extract)
    source = extract / f"{PRODUCT}-{version}-source"
    build = work / "build"
    configure = [
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DTORNE_BUILD_TESTS=OFF",
        "-DTORNE_BUILD_PCSC_IFD=ON",
        f"-DTORNE_CORE_SOURCE_DIR={source / 'third_party/dtv-userland-core'}",
        f"-DTORNE_LIBUSB_SOURCE_ARCHIVE={source / 'third_party/libusb/libusb-1.0.30.tar.bz2'}",
        "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc",
        "-DCMAKE_SHARED_LINKER_FLAGS=-static-libstdc++ -static-libgcc",
    ]
    run(configure, cwd=source)
    run(["cmake", "--build", str(build), "--parallel", str(jobs), "--target",
         "torne_torned", "torne_tornectl", "torne_torne_ts", "torne_pcsc_stage"],
        cwd=source)
    return build


def needed(path: Path) -> list[str]:
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]",
                      output(["readelf", "-d", str(path)]))


def check_dependencies(path: Path) -> None:
    deps = needed(path)
    forbidden = [dep for dep in deps if any(token in dep for token in FORBIDDEN_NEEDED)]
    unexpected = [dep for dep in deps if dep not in ALLOWED_NEEDED and dep not in forbidden]
    if forbidden or unexpected:
        fail(f"unexpected ELF dependency in {path.name}: {deps}")


def interpreter(path: Path) -> str | None:
    match = re.search(r"Requesting program interpreter: ([^\]]+)",
                      output(["readelf", "-l", str(path)]))
    return match.group(1) if match else None


def check_binary(path: Path) -> None:
    check_dependencies(path)
    loader = interpreter(path)
    if loader is None:
        fail(f"missing ELF program interpreter in {path.name}")
    if loader not in ALLOWED_INTERPRETERS:
        fail(f"non-standard ELF program interpreter in {path.name}: {loader}")


def check_ifd(path: Path) -> None:
    check_dependencies(path)
    symbols = output(["nm", "-D", "--defined-only", str(path)])
    for symbol in IFD_SYMBOLS:
        if re.search(rf"\s_?{re.escape(symbol)}$", symbols, re.MULTILINE) is None:
            fail(f"missing IFD symbol: {symbol}")


def build_binary_archive(version: str, product_source: dict[str, str],
                         checkout: dict[str, str], source_map_sha256: str | None,
                         core_commit: str, libusb_version: str, libusb_url: str,
                         libusb_sha256: str, source_root: Path, source_archive_path: Path,
                         build: Path, work: Path, output_dir: Path) -> Path:
    root_name = f"{PRODUCT}-{version}-{PLATFORM}"
    stage = work / root_name
    for sub in ("bin", "ifd", "licenses", "docs"):
        (stage / sub).mkdir(parents=True)

    for name in BINARIES:
        source = build / name
        if not source.is_file():
            fail(f"missing built binary: {source}")
        shutil.copy2(source, stage / "bin" / name)
        check_binary(stage / "bin" / name)

    ifd_stage = build / "pcsc" / "unix"
    ifd = ifd_stage / "libdtv_userland_unix_pcsc_ifd.so"
    if not ifd.is_file():
        fail("missing staged Unix IFD")
    shutil.copy2(ifd, stage / "ifd" / "torne-userland-ifd.so")
    check_ifd(stage / "ifd" / "torne-userland-ifd.so")
    for name in ("COPYING", "UNIX_PCSC_IFD_PROVENANCE.json", "torne.reader.conf.in"):
        source = ifd_stage / name
        if not source.is_file():
            fail(f"missing IFD staging file: {name}")
        shutil.copy2(source, stage / "ifd" / name)

    for source, destination in (
        (source_root / "LICENSE", stage / "licenses" / "LICENSE"),
        (source_root / "NOTICE", stage / "licenses" / "NOTICE"),
        (source_root / "PROVENANCE.md", stage / "licenses" / "PROVENANCE.md"),
        (source_root / "third_party" / "dtv-userland-core" / "LICENSE",
         stage / "licenses" / "LICENSE.dtv-userland-core"),
        (source_root / "third_party" / "libusb" / "COPYING",
         stage / "licenses" / "libusb-COPYING"),
        (source_root / "README.md", stage / "docs" / "README.md"),
        (source_root / "docs" / "cli-framework.md", stage / "docs" / "cli-framework.md"),
    ):
        if not source.is_file():
            fail(f"missing release license/document input: {source}")
        shutil.copy2(source, destination)

    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    metadata = {
        "schema": 1,
        "product": PRODUCT,
        "version": version,
        "platform": PLATFORM,
        "product_source": product_source,
        "checkout": checkout,
        "source_map": ({"path": "SOURCE-MAP.json", "sha256": source_map_sha256}
                       if source_map_sha256 is not None else None),
        "source_commit": product_source["commit"],
        "source_tree": product_source["tree"],
        "core_commit": core_commit,
        "libusb_version": libusb_version,
        "libusb_url": libusb_url,
        "libusb_sha256": libusb_sha256,
        "source_archive": source_archive_path.name,
        "toolchain": {
            "cc": output([cc, "--version"]).splitlines()[0],
            "cxx": output([cxx, "--version"]).splitlines()[0],
            "cmake": output(["cmake", "--version"]).splitlines()[0],
            "glibc": output(["getconf", "GNU_LIBC_VERSION"]),
        },
        "binary_sha256": {name: sha256(stage / "bin" / name) for name in BINARIES},
        "ifd_sha256": sha256(stage / "ifd" / "torne-userland-ifd.so"),
        "ifd_symbols": list(IFD_SYMBOLS),
        "note": "No bit-for-bit reproducibility claim; inputs, toolchain, hashes and rebuild inputs are recorded.",
    }
    write_json(stage / "BUILD-METADATA.json", metadata)
    notice_lines = [
        f"# {PRODUCT} {version} {PLATFORM}",
        "",
        f"Source: {source_archive_path.name}",
        f"Core: Khronos31/dtv-userland-core {core_commit}",
        f"libusb: {libusb_version}, static LGPL-2.1-or-later, {libusb_url}",
        f"libusb source SHA-256: {libusb_sha256}",
        "",
        "The three CLI executables are built from the fixed source and statically link libusb.",
        "The PC/SC IFD is a glibc shared object and does not link libpcsclite.",
        "Product LICENSE/NOTICE/PROVENANCE, Core MIT/GPL notices, libusb COPYING,",
        "and source/rebuild inputs are included. FR30 source and generated images are in the source archive.",
        "No Homebrew/libpcsclite runtime dependency is introduced.",
    ]
    (stage / "DEPENDENCY-NOTICE.txt").write_text("\n".join(notice_lines) + "\n", encoding="utf-8")

    rows = []
    for path in sorted(stage.rglob("*")):
        if path.is_file() and path.name != "SHA256SUMS":
            rows.append(f"{sha256(path)}  {path.relative_to(stage).as_posix()}")
    (stage / "SHA256SUMS").write_text("\n".join(rows) + "\n", encoding="ascii")

    archive_path = output_dir / f"{root_name}.tar.gz"
    deterministic_tar(stage, archive_path)
    return archive_path


def verify_extracted(archive_path: Path, version: str, work: Path) -> None:
    extract = work / "verify"
    extract.mkdir()
    with tarfile.open(archive_path) as archive:
        safe_extract(archive, extract)
    root = extract / f"{PRODUCT}-{version}-{PLATFORM}"
    for name in BINARIES:
        for argument in ("--version", "--help"):
            run([str(root / "bin" / name), argument], cwd=root, capture=True)


def write_outer_checksums(output_dir: Path, archives: list[Path]) -> Path:
    path = output_dir / "SHA256SUMS"
    path.write_text("".join(f"{sha256(item)}  {item.name}\n" for item in sorted(archives)),
                    encoding="ascii")
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--core-source", required=True)
    parser.add_argument("--libusb-source-archive")
    parser.add_argument("--source-only", action="store_true",
                        help="manufacture and verify the source archive without building binaries")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    args = parser.parse_args()

    os.umask(0o022)
    output_dir = Path(args.output).expanduser().resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        fail("--output must be empty or absent")
    output_dir.mkdir(parents=True, exist_ok=True)

    require_clean_source()
    checkout = checkout_identity()
    product_from_map, source_map_sha256 = read_source_map()
    product_source = product_from_map if product_from_map is not None else checkout
    version = read_version()
    core_commit = read_core_commit()
    libusb_version, libusb_url, libusb_sha256 = read_libusb()
    core_source = Path(args.core_source).expanduser().resolve()
    if output(["git", "rev-parse", "HEAD"], cwd=core_source) != core_commit:
        fail("--core-source does not match the fixed Core commit")

    with tempfile.TemporaryDirectory(prefix="torne-release-") as temporary:
        work = Path(temporary)
        if args.libusb_source_archive:
            libusb_archive = Path(args.libusb_source_archive).expanduser().resolve()
            if sha256(libusb_archive) != libusb_sha256:
                fail("libusb source archive checksum mismatch")
        else:
            libusb_archive = work / f"libusb-{libusb_version}.tar.bz2"
            download_libusb(libusb_url, libusb_sha256, libusb_archive)

        source_path, source_root = source_archive(
            version, product_source, checkout, source_map_sha256,
            core_commit, core_source, libusb_archive, libusb_version,
            libusb_url, libusb_sha256, work, output_dir)
        if args.source_only:
            checksums = write_outer_checksums(output_dir, [source_path])
            for item in (source_path, checksums):
                item.chmod(0o644)
            print(f"source_archive={source_path}")
            print(f"checksums={checksums}")
            return 0
        build = build_from_source(version, source_path, work, args.jobs)
        binary_path = build_binary_archive(
            version, product_source, checkout, source_map_sha256, core_commit,
            libusb_version, libusb_url, libusb_sha256, source_root,
            source_path, build, work, output_dir)
        verify_extracted(binary_path, version, work)
        checksums = write_outer_checksums(output_dir, [source_path, binary_path])
        for item in (source_path, binary_path, checksums):
            item.chmod(0o644)

    print(f"source_archive={output_dir / (PRODUCT + '-' + version + '-source.tar.gz')}")
    print(f"binary_archive={output_dir / (PRODUCT + '-' + version + '-' + PLATFORM + '.tar.gz')}")
    print(f"checksums={output_dir / 'SHA256SUMS'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
