#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the source and Linux release archives from fixed inputs."""

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
import plistlib
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PRODUCT = "torne-userland"
DEFAULT_PLATFORM = "linux-glibc-x86_64"
LINUX_PLATFORMS = (
    "linux-glibc-x86_64",
    "linux-glibc-aarch64",
    "linux-musl-x86_64",
    "linux-musl-aarch64",
)
DARWIN_PLATFORM = "darwin-arm64"
WINDOWS_PLATFORM = "windows-x86_64"
DESKTOP_PLATFORMS = (DARWIN_PLATFORM, WINDOWS_PLATFORM)
PLATFORMS = LINUX_PLATFORMS + DESKTOP_PLATFORMS
WINDOWS_EXPORT_NAMES = (
    "SCardConnectA",
    "SCardConnectW",
    "SCardDisconnect",
    "SCardEstablishContext",
    "SCardFreeMemory",
    "SCardGetStatusChangeW",
    "SCardIsValidContext",
    "SCardListReadersA",
    "SCardListReadersW",
    "SCardReleaseContext",
    "SCardStatusW",
    "SCardTransmit",
    "g_rgSCardT1Pci",
)
WINDOWS_ALLOWED_IMPORTS = {
    "advapi32.dll",
    "kernel32.dll",
    "shell32.dll",
    "ws2_32.dll",
    "bcrypt.dll",
    "ucrtbase.dll",
}
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
GLIBC_ALLOWED_NEEDED = {
    "libc.so.6",
    "libm.so.6",
    "libpthread.so.0",
    "librt.so.1",
    "libdl.so.2",
    "ld-linux-x86-64.so.2",
}
MUSL_ALLOWED_NEEDED = {"libc.so"}
FORBIDDEN_NEEDED = (
    "libusb",
    "libpcsclite",
    "libudev",
    "libstdc++",
    "libgcc_s",
    "libc++",
)
# Distro-standard glibc loader aliases. musl CLI archives are fully static.
ALLOWED_INTERPRETERS = {
    "x86_64": frozenset({
        "/lib64/ld-linux-x86-64.so.2",
        "/lib/ld-linux-x86-64.so.2",
        "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
        "/usr/lib64/ld-linux-x86-64.so.2",
        "/usr/lib/ld-linux-x86-64.so.2",
        "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
    }),
    "aarch64": frozenset({
        "/lib/ld-linux-aarch64.so.1",
        "/lib64/ld-linux-aarch64.so.1",
        "/usr/lib/ld-linux-aarch64.so.1",
        "/usr/lib64/ld-linux-aarch64.so.1",
    }),
}
ELF_MACHINE = {"x86_64": "Advanced Micro Devices X86-64", "aarch64": "AArch64"}


def fail(message: str) -> None:
    print(f"release: {message}", file=sys.stderr)
    raise SystemExit(1)


def run(args: list[str], *, cwd: Path | None = None,
        capture: bool = False, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    process_env = None if env is None else {**os.environ, **env}
    return subprocess.run(args, cwd=cwd, check=True, text=True,
                          capture_output=capture, env=process_env)


def output(args: list[str], *, cwd: Path | None = None,
           env: dict[str, str] | None = None) -> str:
    return run(args, cwd=cwd, capture=True, env=env).stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def split_platform(platform: str) -> tuple[str, str]:
    if platform == DARWIN_PLATFORM:
        return "darwin", "arm64"
    if platform == WINDOWS_PLATFORM:
        return "windows", "x86_64"
    match = re.fullmatch(r"linux-(glibc|musl)-(x86_64|aarch64)", platform)
    if match is None:
        fail(f"unsupported platform: {platform}")
    return match.group(1), match.group(2)


def executable_path(value: str) -> Path:
    path = Path(value)
    if path.is_absolute():
        if not path.is_file():
            fail(f"toolchain executable does not exist: {value}")
        return path
    found = shutil.which(value)
    if found is None:
        fail(f"toolchain executable not found: {value}")
    return Path(found)


def load_toolchains(path: str | None) -> dict[str, dict[str, object]]:
    if path is None:
        return {}
    config_path = Path(path).expanduser().resolve()
    try:
        data = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"invalid toolchain config: {error}")
    if not isinstance(data, dict) or data.get("schema") != 1:
        fail("toolchain config must contain schema 1")
    platforms = data.get("platforms")
    if not isinstance(platforms, dict):
        fail("toolchain config must contain a platforms object")
    result: dict[str, dict[str, object]] = {}
    for platform, value in platforms.items():
        if platform not in PLATFORMS:
            fail(f"unsupported platform in toolchain config: {platform}")
        if not isinstance(value, dict):
            fail(f"toolchain entry must be an object: {platform}")
        for key in ("cc", "cxx"):
            if not isinstance(value.get(key), str) or not value[key]:
                fail(f"toolchain {platform}.{key} is missing")
        for key in ("c_flags", "cxx_flags", "exe_linker_flags", "shared_linker_flags"):
            if key in value and not isinstance(value[key], str):
                fail(f"toolchain {platform}.{key} must be a string")
        if "sysroot" in value and value["sysroot"] is not None and not isinstance(value["sysroot"], str):
            fail(f"toolchain {platform}.sysroot must be a string or null")
        if "runner" in value and not isinstance(value["runner"], list):
            fail(f"toolchain {platform}.runner must be a list")
        if "cmake_args" in value and not isinstance(value["cmake_args"], list):
            fail(f"toolchain {platform}.cmake_args must be a list")
        if "cmake_args" in value and not all(isinstance(item, str) for item in value["cmake_args"]):
            fail(f"toolchain {platform}.cmake_args must be a string list")
        for key in ("c_compiler_target", "cxx_compiler_target"):
            if key in value and value[key] is not None and not isinstance(value[key], str):
                fail(f"toolchain {platform}.{key} must be a string or null")
        if "env" in value:
            if not isinstance(value["env"], dict) or not all(
                    isinstance(key, str) and isinstance(item, str)
                    for key, item in value["env"].items()):
                fail(f"toolchain {platform}.env must be a string map")
        result[platform] = dict(value)
    return result


def default_linker_flags(platform: str) -> str:
    # Darwin uses its system C++ runtime. Linux and Windows keep the product's
    # existing static runtime defaults.
    if platform == DARWIN_PLATFORM:
        return ""
    return "-static-libstdc++ -static-libgcc"


def resolve_platform(platform: str, toolchains: dict[str, dict[str, object]]) -> dict[str, object]:
    libc, cpu = split_platform(platform)
    if platform in toolchains:
        values = dict(toolchains[platform])
    elif platform == DEFAULT_PLATFORM:
        values = {"cc": os.environ.get("CC", "cc"), "cxx": os.environ.get("CXX", "c++")}
    else:
        fail(f"--toolchain-config is required for {platform}")
    cc = executable_path(str(values["cc"]))
    cxx = executable_path(str(values["cxx"]))
    runner = values.get("runner", [])
    if runner:
        if not isinstance(runner, list) or not all(isinstance(item, str) for item in runner):
            fail(f"toolchain {platform}.runner must be a string list")
        runner = [str(executable_path(runner[0]))] + list(runner[1:])
    values.update({
        "platform": platform,
        "libc": libc,
        "cpu": cpu,
        "cc": str(cc),
        "cxx": str(cxx),
        "c_flags": str(values.get("c_flags", "")),
        "cxx_flags": str(values.get("cxx_flags", "")),
        "exe_linker_flags": str(values.get("exe_linker_flags", default_linker_flags(platform))),
        "shared_linker_flags": str(values.get("shared_linker_flags", default_linker_flags(platform))),
        "sysroot": values.get("sysroot"),
        "cmake_system_name": values.get("cmake_system_name"),
        "system_processor": str(values.get("system_processor", cpu)),
        "c_compiler_target": values.get("c_compiler_target"),
        "cxx_compiler_target": values.get("cxx_compiler_target"),
        "readobj": values.get("readobj"),
        "cmake_args": list(values.get("cmake_args", [])),
        "runner": runner,
        "run_checks": bool(values.get("run_checks", cpu == "x86_64")),
        "emulated": bool(values.get("emulated", False)),
        "build_kind": str(values.get("build_kind", "native" if cpu == "x86_64" else "cross")),
        "env": dict(values.get("env", {})),
    })
    return values


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


def read_source_archive_metadata(source_archive_path: Path, version: str) -> dict[str, object]:
    with tarfile.open(source_archive_path) as archive:
        members = [m for m in archive.getmembers()
                   if m.name.endswith("/SOURCE-METADATA.json")]
        if len(members) != 1:
            fail("source archive must contain exactly one SOURCE-METADATA.json")
        stream = archive.extractfile(members[0])
        if stream is None:
            fail("cannot read SOURCE-METADATA.json")
        try:
            data = json.loads(stream.read().decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            fail(f"invalid source archive metadata: {error}")
    if not isinstance(data, dict) or data.get("schema") != 1 or data.get("product") != PRODUCT:
        fail("source archive metadata is not a torne-userland schema 1 record")
    if data.get("version") != version:
        fail("source archive version does not match this helper")
    for key in ("product_source", "checkout"):
        value = data.get(key)
        if not isinstance(value, dict) or not all(
                isinstance(value.get(field), str) and value[field]
                for field in ("repository", "commit", "tree")):
            fail(f"source archive metadata {key} is invalid")
    for key in ("core_commit", "libusb_version", "libusb_url", "libusb_sha256"):
        if not isinstance(data.get(key), str) or not data[key]:
            fail(f"source archive metadata {key} is missing")
    return data


def extract_source(version: str, source_archive_path: Path, work: Path) -> Path:
    extract = work / "source-build"
    extract.mkdir()
    with tarfile.open(source_archive_path) as archive:
        safe_extract(archive, extract)
    return extract / f"{PRODUCT}-{version}-source"


def build_platform(version: str, source: Path, spec: dict[str, object],
                   work: Path, jobs: int) -> Path:
    platform = str(spec["platform"])
    build = work / f"build-{platform}"
    configure = [
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DTORNE_BUILD_TESTS=OFF",
        f"-DTORNE_CORE_SOURCE_DIR={source / 'third_party/dtv-userland-core'}",
        f"-DTORNE_LIBUSB_SOURCE_ARCHIVE={source / 'third_party/libusb/libusb-1.0.30.tar.bz2'}",
        f"-DCMAKE_C_COMPILER={spec['cc']}",
        f"-DCMAKE_CXX_COMPILER={spec['cxx']}",
        f"-DCMAKE_SYSTEM_PROCESSOR={spec['system_processor']}",
        f"-DCMAKE_EXE_LINKER_FLAGS={spec['exe_linker_flags']}",
        f"-DCMAKE_SHARED_LINKER_FLAGS={spec['shared_linker_flags']}",
    ]
    if platform == DARWIN_PLATFORM:
        configure += [
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DTORNE_BUILD_PCSC_IFD=ON",
            "-DTORNE_BUILD_WINSCARD=OFF",
        ]
        targets = ["torne_torned", "torne_tornectl", "torne_torne_ts",
                   "torne_pcsc_bundle"]
    elif platform == WINDOWS_PLATFORM:
        configure += [
            "-DTORNE_BUILD_PCSC_IFD=OFF",
            "-DTORNE_BUILD_WINSCARD=ON",
        ]
        targets = ["torne_torned", "torne_tornectl", "torne_torne_ts",
                   "torne_pcsc_stage"]
    else:
        configure += [
            "-DTORNE_BUILD_PCSC_IFD=ON",
            "-DTORNE_BUILD_WINSCARD=OFF",
        ]
        targets = ["torne_torned", "torne_tornectl", "torne_torne_ts",
                   "torne_pcsc_stage"]
    if spec.get("cmake_system_name"):
        configure.append(f"-DCMAKE_SYSTEM_NAME={spec['cmake_system_name']}")
    if spec.get("sysroot"):
        configure.append(f"-DCMAKE_SYSROOT={spec['sysroot']}")
    if spec.get("c_compiler_target"):
        configure.append(f"-DCMAKE_C_COMPILER_TARGET={spec['c_compiler_target']}")
    if spec.get("cxx_compiler_target"):
        configure.append(f"-DCMAKE_CXX_COMPILER_TARGET={spec['cxx_compiler_target']}")
    if spec.get("c_flags"):
        configure.append(f"-DCMAKE_C_FLAGS={spec['c_flags']}")
    if spec.get("cxx_flags"):
        configure.append(f"-DCMAKE_CXX_FLAGS={spec['cxx_flags']}")
    configure.extend(str(item) for item in spec.get("cmake_args", []))
    run(configure, cwd=source, env=spec["env"])
    run(["cmake", "--build", str(build), "--parallel", str(jobs), "--target", *targets],
        cwd=source, env=spec["env"])
    return build


def needed(path: Path) -> list[str]:
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]",
                      output(["readelf", "-d", str(path)]))


def interpreter(path: Path) -> str | None:
    match = re.search(r"Requesting program interpreter: ([^\]]+)",
                      output(["readelf", "-l", str(path)]))
    return match.group(1) if match else None


def elf_identity(path: Path) -> tuple[str, str, str]:
    text = output(["readelf", "-h", str(path)])
    elf_class = re.search(r"Class:\s+(\S+)", text)
    elf_type = re.search(r"Type:\s+(\S+)", text)
    machine = re.search(r"Machine:\s+(.+)", text)
    if elf_class is None or elf_type is None or machine is None:
        fail(f"cannot read ELF identity: {path.name}")
    return elf_class.group(1), elf_type.group(1), machine.group(1).strip()


def check_elf_identity(path: Path, spec: dict[str, object]) -> None:
    elf_class, _elf_type, machine = elf_identity(path)
    if elf_class != "ELF64":
        fail(f"unexpected ELF class in {path.name}: {elf_class}")
    expected = ELF_MACHINE[str(spec["cpu"])]
    if machine != expected:
        fail(f"unexpected ELF machine in {path.name}: {machine}")


def check_dependencies(path: Path, spec: dict[str, object]) -> list[str]:
    deps = needed(path)
    libc = str(spec["libc"])
    allowed = GLIBC_ALLOWED_NEEDED if libc == "glibc" else MUSL_ALLOWED_NEEDED
    forbidden = [dep for dep in deps if any(token in dep for token in FORBIDDEN_NEEDED)]
    unexpected = [dep for dep in deps if dep not in allowed and dep not in forbidden]
    if forbidden or unexpected:
        fail(f"unexpected ELF dependency in {path.name}: {deps}")
    return deps


def check_binary(path: Path, spec: dict[str, object]) -> None:
    check_elf_identity(path, spec)
    deps = check_dependencies(path, spec)
    loader = interpreter(path)
    if str(spec["libc"]) == "musl":
        if deps or loader is not None:
            fail(f"musl CLI is not fully static: {path.name}")
        return
    if loader is None:
        fail(f"missing ELF program interpreter in {path.name}")
    if loader not in ALLOWED_INTERPRETERS[str(spec["cpu"])]:
        fail(f"non-standard ELF program interpreter in {path.name}: {loader}")


def check_ifd(path: Path, spec: dict[str, object]) -> None:
    _elf_class, elf_type, _machine = elf_identity(path)
    check_elf_identity(path, spec)
    if elf_type != "DYN":
        fail(f"IFD is not a shared object: {path.name}")
    check_dependencies(path, spec)
    if interpreter(path) is not None:
        fail(f"IFD unexpectedly has a program interpreter: {path.name}")
    symbols = output(["nm", "-D", "--defined-only", str(path)])
    for symbol in IFD_SYMBOLS:
        if re.search(rf"\s_?{re.escape(symbol)}$", symbols, re.MULTILINE) is None:
            fail(f"missing IFD symbol: {symbol}")


def compiler_metadata(value: str, env: dict[str, str]) -> dict[str, str]:
    path = executable_path(value)
    return {
        "path": str(path),
        "sha256": sha256(path),
        "version": output([str(path), "--version"], env=env).splitlines()[0],
    }


MAC_ALLOWED_DEPENDENCIES = (
    "/System/Library/Frameworks/",
    "/usr/lib/libc++.1.dylib",
    "/usr/lib/libc++abi.dylib",
    "/usr/lib/libSystem.B.dylib",
    "/usr/lib/libobjc.A.dylib",
)


def macos_dependencies(path: Path) -> list[str]:
    text = output(["otool", "-L", str(path)])
    deps = []
    for line in text.splitlines()[1:]:
        match = re.match(r"\s+([^ ]+) \(compatibility", line)
        if match:
            deps.append(match.group(1))
    return deps


def check_macos_macho(path: Path, spec: dict[str, object]) -> list[str]:
    description = output(["file", str(path)])
    if "Mach-O 64-bit" not in description or "arm64" not in description:
        fail(f"not an arm64 Mach-O file: {path.name}: {description}")
    deps = macos_dependencies(path)
    for dep in deps:
        if not dep.startswith(MAC_ALLOWED_DEPENDENCIES):
            fail(f"non-system macOS dependency in {path.name}: {dep}")
    return deps


def check_macos_ifd_bundle(bundle: Path) -> dict[str, object]:
    executable_name = "torne-userland-ifd"
    info_path = bundle / "Contents" / "Info.plist"
    executable = bundle / "Contents" / "MacOS" / executable_name
    resources = bundle / "Contents" / "Resources"
    for path in (info_path, executable, resources / "COPYING",
                 resources / "UNIX_PCSC_IFD_PROVENANCE.json",
                 resources / "LICENSE", resources / "LICENSE.dtv-userland-core"):
        if not path.is_file():
            fail(f"missing macOS IFD bundle file: {path}")
    info = plistlib.loads(info_path.read_bytes())
    if info.get("CFBundleExecutable") != executable_name:
        fail("macOS IFD CFBundleExecutable does not match bundle executable")
    description = output(["file", str(executable)])
    if "Mach-O 64-bit" not in description or "arm64" not in description:
        fail(f"IFD bundle executable is not arm64 Mach-O: {description}")
    deps = macos_dependencies(executable)
    for dep in deps:
        if dep == "@rpath/libdtv_userland_unix_pcsc_ifd.dylib":
            continue
        if not dep.startswith(MAC_ALLOWED_DEPENDENCIES):
            fail(f"non-system macOS IFD dependency: {dep}")
    symbols = output(["nm", "-gU", str(executable)])
    for symbol in IFD_SYMBOLS:
        if re.search(rf"\s_?{re.escape(symbol)}$", symbols, re.MULTILINE) is None:
            fail(f"missing macOS IFD symbol: {symbol}")
    return {
        "CFBundleExecutable": executable_name,
        "executable_sha256": sha256(executable),
        "Info.plist_sha256": sha256(info_path),
        "dependencies": deps,
        "symbols": list(IFD_SYMBOLS),
    }


def windows_info(path: Path, spec: dict[str, object]) -> tuple[list[str], list[str]]:
    readobj = spec.get("readobj")
    if not readobj:
        fail("--toolchain-config readobj is required for Windows PE checks")
    readobj_path = executable_path(str(readobj))
    headers = output([str(readobj_path), "--file-headers", str(path)])
    if "Format: COFF-x86-64" not in headers or "Arch: x86_64" not in headers:
        fail(f"not an x86_64 COFF file: {path.name}")
    imports_text = output([str(readobj_path), "--coff-imports", str(path)])
    imports = re.findall(r"^  Name: ([^ ]+)$", imports_text, re.MULTILINE)
    exports_text = output([str(readobj_path), "--coff-exports", str(path)])
    exports = re.findall(r"^  Name: ([^ ]+)$", exports_text, re.MULTILINE)
    return imports, exports


def check_windows_pe(path: Path, spec: dict[str, object]) -> list[str]:
    imports, _exports = windows_info(path, spec)
    for name in imports:
        lowered = name.lower()
        if lowered in WINDOWS_ALLOWED_IMPORTS or lowered.startswith("api-ms-win-crt-"):
            continue
        fail(f"unexpected Windows import in {path.name}: {name}")
    if any(token in name.lower() for name in imports for token in
           ("libusb", "libstdc++", "libgcc", "libwinpthread", "libc++")):
        fail(f"non-OS runtime import in {path.name}")
    return imports


def check_windows_dll(path: Path, spec: dict[str, object]) -> list[str]:
    imports, exports = windows_info(path, spec)
    check_windows_pe(path, spec)
    if set(exports) != set(WINDOWS_EXPORT_NAMES) or len(exports) != 13:
        fail(f"unexpected WinSCard exports: {exports}")
    return imports


def deterministic_zip(source: Path, destination: Path, root_name: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED,
                         allowZip64=False) as archive:
        for path in sorted(p for p in source.rglob("*") if p.is_file()):
            if path.is_symlink():
                fail(f"symlink in zip source: {path}")
            relative = path.relative_to(source).as_posix()
            info = zipfile.ZipInfo(f"{root_name}/{relative}", (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100644 & 0xFFFF) << 16
            archive.writestr(info, path.read_bytes())


def build_binary_archive(version: str, spec: dict[str, object],
                         product_source: dict[str, str],
                         checkout: dict[str, str], source_map_sha256: str | None,
                         core_commit: str, libusb_version: str, libusb_url: str,
                         libusb_sha256: str, source_root: Path,
                         source_archive_path: Path, source_archive_sha256: str,
                         build: Path, work: Path, output_dir: Path) -> Path:
    platform = str(spec["platform"])
    root_name = f"{PRODUCT}-{version}-{platform}"
    stage = work / root_name
    for sub in ("bin", "ifd", "licenses", "docs"):
        (stage / sub).mkdir(parents=True)

    binary_needed: dict[str, list[str]] = {}
    binary_interpreter: dict[str, str | None] = {}
    for name in BINARIES:
        source = build / name
        if not source.is_file():
            fail(f"missing built binary: {source}")
        shutil.copy2(source, stage / "bin" / name)
        check_binary(stage / "bin" / name, spec)
        binary_needed[name] = needed(stage / "bin" / name)
        binary_interpreter[name] = interpreter(stage / "bin" / name)

    ifd_stage = build / "pcsc" / "unix"
    ifd = ifd_stage / "libdtv_userland_unix_pcsc_ifd.so"
    if not ifd.is_file():
        fail("missing staged Unix IFD")
    shutil.copy2(ifd, stage / "ifd" / "torne-userland-ifd.so")
    check_ifd(stage / "ifd" / "torne-userland-ifd.so", spec)
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

    verification = "native" if spec["run_checks"] and not spec["emulated"] else (
        "emulated via QEMU" if spec["run_checks"] and spec["emulated"] else "not run")
    metadata = {
        "schema": 1,
        "product": PRODUCT,
        "version": version,
        "platform": platform,
        "libc": spec["libc"],
        "cpu": spec["cpu"],
        "build_kind": spec["build_kind"],
        "emulated": bool(spec["emulated"]),
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
        "source_archive_sha256": source_archive_sha256,
        "toolchain": {
            "cc": compiler_metadata(str(spec["cc"]), spec["env"]),
            "cxx": compiler_metadata(str(spec["cxx"]), spec["env"]),
            "c_flags": spec["c_flags"],
            "cxx_flags": spec["cxx_flags"],
            "sysroot": spec["sysroot"],
            "exe_linker_flags": spec["exe_linker_flags"],
            "shared_linker_flags": spec["shared_linker_flags"],
            "system_processor": spec["system_processor"],
            "c_compiler_target": spec["c_compiler_target"],
            "cxx_compiler_target": spec["cxx_compiler_target"],
            "env": spec["env"],
            "cmake": output(["cmake", "--version"]).splitlines()[0],
            "host_glibc": output(["getconf", "GNU_LIBC_VERSION"]),
        },
        "binary_sha256": {name: sha256(stage / "bin" / name) for name in BINARIES},
        "binary_needed": binary_needed,
        "binary_interpreter": binary_interpreter,
        "ifd_sha256": sha256(stage / "ifd" / "torne-userland-ifd.so"),
        "ifd_needed": needed(stage / "ifd" / "torne-userland-ifd.so"),
        "ifd_symbols": list(IFD_SYMBOLS),
        "verification": {
            "elf_identity": "checked",
            "elf_dependencies": "checked",
            "ifd_symbols": "checked 10",
            "ifd_libpcsclite_free": "checked",
            "post_extract_help_version": verification,
        },
        "note": "No bit-for-bit reproducibility claim; inputs, toolchain, hashes and rebuild inputs are recorded.",
    }
    write_json(stage / "BUILD-METADATA.json", metadata)
    libc = str(spec["libc"])
    cli_linkage = "fully static musl" if libc == "musl" else "glibc PIE"
    notice_lines = [
        f"# {PRODUCT} {version} {platform}",
        "",
        f"Source: {source_archive_path.name}",
        f"Source archive SHA-256: {source_archive_sha256}",
        f"Core: Khronos31/dtv-userland-core {core_commit}",
        f"libusb: {libusb_version}, static LGPL-2.1-or-later, {libusb_url}",
        f"libusb source SHA-256: {libusb_sha256}",
        "",
        f"The three CLI executables are {cli_linkage} and statically link libusb.",
        f"The PC/SC IFD is a {libc} shared object and does not link libpcsclite.",
        "Product LICENSE/NOTICE/PROVENANCE, Core MIT/GPL notices, libusb COPYING,",
        "and source/rebuild inputs are included. FR30 source and generated images are in the source archive.",
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


def toolchain_metadata(spec: dict[str, object]) -> dict[str, object]:
    return {
        "cc": compiler_metadata(str(spec["cc"]), spec["env"]),
        "cxx": compiler_metadata(str(spec["cxx"]), spec["env"]),
        "c_flags": spec["c_flags"],
        "cxx_flags": spec["cxx_flags"],
        "sysroot": spec["sysroot"],
        "exe_linker_flags": spec["exe_linker_flags"],
        "shared_linker_flags": spec["shared_linker_flags"],
        "system_processor": spec["system_processor"],
        "c_compiler_target": spec["c_compiler_target"],
        "cxx_compiler_target": spec["cxx_compiler_target"],
        "env": spec["env"],
        "cmake": output(["cmake", "--version"]).splitlines()[0],
    }


def copy_release_docs(source_root: Path, stage: Path) -> None:
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
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def write_inner_checksums(stage: Path) -> None:
    rows = []
    for path in sorted(stage.rglob("*")):
        if path.is_file() and path.name != "SHA256SUMS":
            rows.append(f"{sha256(path)}  {path.relative_to(stage).as_posix()}")
    (stage / "SHA256SUMS").write_text("\n".join(rows) + "\n", encoding="ascii")


def build_darwin_archive(version: str, spec: dict[str, object],
                         product_source: dict[str, str],
                         checkout: dict[str, str], source_map_sha256: str | None,
                         core_commit: str, libusb_version: str, libusb_url: str,
                         libusb_sha256: str, source_root: Path,
                         source_archive_path: Path, source_archive_sha256: str,
                         build: Path, work: Path, output_dir: Path) -> Path:
    root_name = f"{PRODUCT}-{version}-{DARWIN_PLATFORM}"
    stage = work / root_name
    for sub in ("bin", "ifd", "licenses", "docs"):
        (stage / sub).mkdir(parents=True)
    binary_hashes = {}
    binary_dependencies = {}
    for name in BINARIES:
        source = build / name
        if not source.is_file():
            fail(f"missing built binary: {source}")
        shutil.copy2(source, stage / "bin" / name)
        binary_hashes[name] = sha256(stage / "bin" / name)
        binary_dependencies[name] = check_macos_macho(stage / "bin" / name, spec)
    bundle_source = build / "pcsc" / "torne-userland-ifd.bundle"
    if not bundle_source.is_dir():
        fail("missing macOS IFD bundle")
    bundle = stage / "ifd" / "torne-userland-ifd.bundle"
    shutil.copytree(bundle_source, bundle)
    bundle_info = check_macos_ifd_bundle(bundle)
    shutil.copy2(source_root / "packaging" / "pcsc" / "torne.reader.conf.in",
                 stage / "ifd" / "torne.reader.conf.in")
    copy_release_docs(source_root, stage)
    metadata = {
        "schema": 1,
        "product": PRODUCT,
        "version": version,
        "platform": DARWIN_PLATFORM,
        "libc": "darwin",
        "cpu": "arm64",
        "build_kind": spec["build_kind"],
        "emulated": False,
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
        "source_archive_sha256": source_archive_sha256,
        "toolchain": toolchain_metadata(spec),
        "binary_sha256": binary_hashes,
        "binary_dependencies": binary_dependencies,
        "ifd_bundle": bundle_info,
        "verification": {
            "macho_identity": "checked arm64",
            "system_dependencies": "checked",
            "ifd_bundle": "CFBundleExecutable/10 symbols/dependencies checked",
            "os_registration": "not performed",
            "post_extract_help_version": "native" if spec["run_checks"] else "not run",
        },
        "note": "No bit-for-bit reproducibility claim; inputs, toolchain, hashes and rebuild inputs are recorded.",
    }
    write_json(stage / "BUILD-METADATA.json", metadata)
    (stage / "DEPENDENCY-NOTICE.txt").write_text(
        "\n".join([
            f"# {PRODUCT} {version} {DARWIN_PLATFORM}",
            "",
            f"Source: {source_archive_path.name}",
            f"Source archive SHA-256: {source_archive_sha256}",
            f"Core: Khronos31/dtv-userland-core {core_commit}",
            f"libusb: {libusb_version}, static LGPL-2.1-or-later, {libusb_url}",
            f"libusb source SHA-256: {libusb_sha256}",
            "",
            "The three CLI executables are native arm64 Mach-O programs.",
            "The PC/SC IFD is a candidate bundle and is not registered with the OS.",
            "Dependencies are Apple system frameworks/runtime only; no Homebrew or libpcsclite dependency is introduced.",
            "Product LICENSE/NOTICE/PROVENANCE, Core MIT/GPL notices, libusb COPYING,",
            "and source/rebuild inputs are included.",
        ]) + "\n", encoding="utf-8")
    write_inner_checksums(stage)
    archive_path = output_dir / f"{root_name}.tar.gz"
    deterministic_tar(stage, archive_path)
    return archive_path


def build_windows_archive(version: str, spec: dict[str, object],
                          product_source: dict[str, str],
                          checkout: dict[str, str], source_map_sha256: str | None,
                          core_commit: str, libusb_version: str, libusb_url: str,
                          libusb_sha256: str, source_root: Path,
                          source_archive_path: Path, source_archive_sha256: str,
                          build: Path, work: Path, output_dir: Path) -> Path:
    root_name = f"{PRODUCT}-{version}-{WINDOWS_PLATFORM}"
    stage = work / root_name
    for sub in ("bin", "winscard", "licenses", "docs"):
        (stage / sub).mkdir(parents=True)
    binary_hashes = {}
    binary_imports = {}
    for name in ("torned", "tornectl", "torne-ts"):
        source = build / f"{name}.exe"
        if not source.is_file():
            fail(f"missing built binary: {source}")
        destination = stage / "bin" / f"{name}.exe"
        shutil.copy2(source, destination)
        binary_hashes[name] = sha256(destination)
        binary_imports[name] = check_windows_pe(destination, spec)
    stage_source = build / "pcsc" / "windows"
    dll_source = stage_source / "WinSCard.dll"
    if not dll_source.is_file():
        fail("missing staged WinSCard.dll")
    dll = stage / "winscard" / "WinSCard.dll"
    shutil.copy2(dll_source, dll)
    dll_imports = check_windows_dll(dll, spec)
    for name in ("COPYING", "README.md", "LICENSE.dtv-userland-core",
                 "COPYING.MinGW-w64-runtime.txt"):
        source = stage_source / name
        if not source.is_file():
            fail(f"missing Windows stage file: {name}")
        shutil.copy2(source, stage / "winscard" / name)
    copy_release_docs(source_root, stage)
    shutil.copy2(stage / "winscard" / "COPYING.MinGW-w64-runtime.txt",
                 stage / "licenses" / "COPYING.MinGW-w64-runtime.txt")
    metadata = {
        "schema": 1,
        "product": PRODUCT,
        "version": version,
        "platform": WINDOWS_PLATFORM,
        "libc": "windows-ucrt",
        "cpu": "x86_64",
        "build_kind": spec["build_kind"],
        "emulated": False,
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
        "source_archive_sha256": source_archive_sha256,
        "toolchain": toolchain_metadata(spec),
        "binary_sha256": binary_hashes,
        "binary_imports": binary_imports,
        "winscard_sha256": sha256(dll),
        "winscard_imports": dll_imports,
        "winscard_exports": list(WINDOWS_EXPORT_NAMES),
        "verification": {
            "pe_identity": "checked x86-64",
            "system_ucrt_imports": "checked",
            "winscard_exports": "checked 13",
            "native_run": "not performed on this host",
        },
        "note": "No bit-for-bit reproducibility claim; inputs, toolchain, hashes and rebuild inputs are recorded.",
    }
    write_json(stage / "BUILD-METADATA.json", metadata)
    (stage / "DEPENDENCY-NOTICE.txt").write_text(
        "\n".join([
            f"# {PRODUCT} {version} {WINDOWS_PLATFORM}",
            "",
            f"Source: {source_archive_path.name}",
            f"Source archive SHA-256: {source_archive_sha256}",
            f"Core: Khronos31/dtv-userland-core {core_commit}",
            f"libusb: {libusb_version}, static LGPL-2.1-or-later, {libusb_url}",
            f"libusb source SHA-256: {libusb_sha256}",
            "",
            "The three CLI executables and WinSCard.dll use the official llvm-mingw UCRT build.",
            "They import only Windows system/UCRT libraries and do not require libusb/libstdc++/libgcc runtime DLLs.",
            "WinSCard.dll is app-local; System32 and OS reader registration are not modified.",
            "Product LICENSE/NOTICE/PROVENANCE, Core MIT/GPL notices, libusb COPYING,",
            "llvm-mingw runtime notice, and source/rebuild inputs are included.",
        ]) + "\n", encoding="utf-8")
    write_inner_checksums(stage)
    archive_path = output_dir / f"{root_name}.zip"
    deterministic_zip(stage, archive_path, root_name)
    return archive_path


def safe_extract_zip(archive_path: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive_path) as archive:
        for info in archive.infolist():
            name = info.filename
            if name.startswith("/") or "\\" in name:
                fail(f"unsafe zip path: {name}")
            parts = Path(name).parts
            if ".." in parts:
                fail(f"unsupported zip member: {name}")
            archive.extract(info, destination)


def verify_inner_checksums(root: Path) -> None:
    listed = set()
    sums = root / "SHA256SUMS"
    for line in sums.read_text(encoding="ascii").splitlines():
        digest, separator, name = line.partition("  ")
        if not separator or not name:
            fail(f"invalid inner checksum line: {line}")
        path = root / name
        if not path.is_file() or sha256(path) != digest:
            fail(f"inner checksum mismatch: {name}")
        listed.add(name)
    present = {path.relative_to(root).as_posix() for path in root.rglob("*")
               if path.is_file() and path.name != "SHA256SUMS"}
    if listed != present:
        fail("inner SHA256SUMS does not cover the extracted archive exactly")


def verify_extracted(archive_path: Path, version: str, spec: dict[str, object],
                     work: Path) -> None:
    platform = str(spec["platform"])
    extract = work / f"verify-{platform}"
    extract.mkdir()
    if platform == WINDOWS_PLATFORM:
        safe_extract_zip(archive_path, extract)
        root = extract / f"{PRODUCT}-{version}-{WINDOWS_PLATFORM}"
        required = [
            *[root / "bin" / f"{name}.exe" for name in BINARIES],
            root / "winscard" / "WinSCard.dll",
            root / "winscard" / "COPYING",
            root / "winscard" / "README.md",
            root / "winscard" / "LICENSE.dtv-userland-core",
            root / "winscard" / "COPYING.MinGW-w64-runtime.txt",
            root / "licenses" / "LICENSE",
            root / "licenses" / "NOTICE",
            root / "licenses" / "PROVENANCE.md",
            root / "licenses" / "LICENSE.dtv-userland-core",
            root / "licenses" / "libusb-COPYING",
            root / "docs" / "README.md",
            root / "docs" / "cli-framework.md",
            root / "BUILD-METADATA.json",
            root / "DEPENDENCY-NOTICE.txt",
            root / "SHA256SUMS",
        ]
        for path in required:
            if not path.is_file():
                fail(f"missing extracted release file: {path.relative_to(extract)}")
        verify_inner_checksums(root)
        return
    with tarfile.open(archive_path) as archive:
        safe_extract(archive, extract)
    root = extract / f"{PRODUCT}-{version}-{platform}"
    if platform == DARWIN_PLATFORM:
        required = [
            *[root / "bin" / name for name in BINARIES],
            root / "ifd" / "torne-userland-ifd.bundle" / "Contents" / "Info.plist",
            root / "ifd" / "torne-userland-ifd.bundle" / "Contents" / "MacOS" /
            "torne-userland-ifd",
            root / "ifd" / "torne.reader.conf.in",
            root / "licenses" / "LICENSE",
            root / "licenses" / "NOTICE",
            root / "licenses" / "PROVENANCE.md",
            root / "licenses" / "LICENSE.dtv-userland-core",
            root / "licenses" / "libusb-COPYING",
            root / "docs" / "README.md",
            root / "docs" / "cli-framework.md",
            root / "BUILD-METADATA.json",
            root / "DEPENDENCY-NOTICE.txt",
            root / "SHA256SUMS",
        ]
    else:
        required = [
            *[root / "bin" / name for name in BINARIES],
            root / "ifd" / "torne-userland-ifd.so",
            root / "ifd" / "COPYING",
            root / "ifd" / "UNIX_PCSC_IFD_PROVENANCE.json",
            root / "ifd" / "torne.reader.conf.in",
            root / "licenses" / "LICENSE",
            root / "licenses" / "NOTICE",
            root / "licenses" / "PROVENANCE.md",
            root / "licenses" / "LICENSE.dtv-userland-core",
            root / "licenses" / "libusb-COPYING",
            root / "docs" / "README.md",
            root / "docs" / "cli-framework.md",
            root / "BUILD-METADATA.json",
            root / "DEPENDENCY-NOTICE.txt",
            root / "SHA256SUMS",
        ]
    for path in required:
        if not path.is_file():
            fail(f"missing extracted release file: {path.relative_to(extract)}")
    verify_inner_checksums(root)
    if platform == DARWIN_PLATFORM:
        check_macos_ifd_bundle(root / "ifd" / "torne-userland-ifd.bundle")
    if not spec["run_checks"]:
        return
    runner = list(spec["runner"])
    for name in BINARIES:
        for argument in ("--version", "--help"):
            run(runner + [str(root / "bin" / name), argument], cwd=root, capture=True)


def write_outer_checksums(output_dir: Path, archives: list[Path]) -> Path:
    path = output_dir / "SHA256SUMS"
    path.write_text("".join(f"{sha256(item)}  {item.name}\n" for item in sorted(archives)),
                    encoding="ascii")
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--core-source")
    parser.add_argument("--source-archive",
                        help="fixed source archive to build from instead of creating a new one")
    parser.add_argument("--libusb-source-archive")
    parser.add_argument("--linux-platform", action="append", choices=LINUX_PLATFORMS,
                        help="Linux archive platform; repeat to manufacture several")
    parser.add_argument("--linux-matrix", action="store_true",
                        help="manufacture all four Linux glibc/musl x86_64/aarch64 archives")
    parser.add_argument("--darwin-arm64", action="store_true",
                        help="manufacture the macOS arm64 archive (must run on macOS)")
    parser.add_argument("--windows-x86_64", action="store_true",
                        help="manufacture the Windows x86_64 archive with llvm-mingw/UCRT")
    parser.add_argument("--toolchain-config",
                        help="schema-1 JSON mapping each selected platform to its fixed toolchain")
    parser.add_argument("--source-only", action="store_true",
                        help="manufacture and verify the source archive without building binaries")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    args = parser.parse_args()

    if args.linux_matrix and args.linux_platform:
        fail("--linux-matrix and --linux-platform are mutually exclusive")
    platforms = []
    if args.linux_matrix:
        platforms.extend(LINUX_PLATFORMS)
    elif args.linux_platform:
        platforms.extend(args.linux_platform)
    if args.darwin_arm64:
        platforms.append(DARWIN_PLATFORM)
    if args.windows_x86_64:
        platforms.append(WINDOWS_PLATFORM)
    if not platforms:
        platforms = [DEFAULT_PLATFORM]
    if len(set(platforms)) != len(platforms):
        fail("duplicate platform selection")
    if DARWIN_PLATFORM in platforms and sys.platform != "darwin":
        fail("--darwin-arm64 must be built on macOS")
    if not args.source_only and platforms != [DEFAULT_PLATFORM] and not args.toolchain_config:
        fail("--toolchain-config is required for non-default platforms")

    os.umask(0o022)
    output_dir = Path(args.output).expanduser().resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        fail("--output must be empty or absent")
    output_dir.mkdir(parents=True, exist_ok=True)
    version = read_version()
    toolchains = load_toolchains(args.toolchain_config)
    specs = [] if args.source_only else [resolve_platform(platform, toolchains)
                                         for platform in platforms]

    with tempfile.TemporaryDirectory(prefix="torne-release-") as temporary:
        work = Path(temporary)
        if args.source_archive:
            input_source = Path(args.source_archive).expanduser().resolve()
            if not input_source.is_file():
                fail("--source-archive does not exist")
            source_metadata = read_source_archive_metadata(input_source, version)
            product_source = source_metadata["product_source"]
            checkout = source_metadata["checkout"]
            source_map = source_metadata.get("source_map")
            source_map_sha256 = source_map.get("sha256") if isinstance(source_map, dict) else None
            core_commit = source_metadata["core_commit"]
            libusb_version = source_metadata["libusb_version"]
            libusb_url = source_metadata["libusb_url"]
            libusb_sha256 = source_metadata["libusb_sha256"]
            source_path = output_dir / input_source.name
            if input_source.resolve() != source_path.resolve():
                shutil.copy2(input_source, source_path)
            source_sha256 = sha256(source_path)
            build_source = extract_source(version, source_path, work)
            source_root = build_source
        else:
            if not args.core_source:
                fail("--core-source is required unless --source-archive is supplied")
            require_clean_source()
            checkout = checkout_identity()
            product_from_map, source_map_sha256 = read_source_map()
            product_source = product_from_map if product_from_map is not None else checkout
            core_commit = read_core_commit()
            libusb_version, libusb_url, libusb_sha256 = read_libusb()
            core_source = Path(args.core_source).expanduser().resolve()
            if output(["git", "rev-parse", "HEAD"], cwd=core_source) != core_commit:
                fail("--core-source does not match the fixed Core commit")
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
            source_sha256 = sha256(source_path)
            build_source = extract_source(version, source_path, work)

        if args.source_only:
            checksums = write_outer_checksums(output_dir, [source_path])
            for item in (source_path, checksums):
                item.chmod(0o644)
            print(f"source_archive={source_path}")
            print(f"checksums={checksums}")
            return 0

        archives = [source_path]
        for spec in specs:
            build = build_platform(version, build_source, spec, work, args.jobs)
            platform = str(spec["platform"])
            if platform == DARWIN_PLATFORM:
                binary_path = build_darwin_archive(
                    version, spec, product_source, checkout, source_map_sha256,
                    core_commit, libusb_version, libusb_url, libusb_sha256,
                    source_root, source_path, source_sha256, build, work, output_dir)
            elif platform == WINDOWS_PLATFORM:
                binary_path = build_windows_archive(
                    version, spec, product_source, checkout, source_map_sha256,
                    core_commit, libusb_version, libusb_url, libusb_sha256,
                    source_root, source_path, source_sha256, build, work, output_dir)
            else:
                binary_path = build_binary_archive(
                    version, spec, product_source, checkout, source_map_sha256,
                    core_commit, libusb_version, libusb_url, libusb_sha256,
                    source_root, source_path, source_sha256, build, work, output_dir)
            verify_extracted(binary_path, version, spec, work)
            archives.append(binary_path)
        checksums = write_outer_checksums(output_dir, archives)
        for item in [*archives, checksums]:
            item.chmod(0o644)

    print(f"source_archive={output_dir / (PRODUCT + '-' + version + '-source.tar.gz')}")
    for platform in platforms:
        suffix = "zip" if platform == WINDOWS_PLATFORM else "tar.gz"
        print(f"binary_archive[{platform}]={output_dir / (PRODUCT + '-' + version + '-' + platform + '.' + suffix)}")
    print(f"checksums={output_dir / 'SHA256SUMS'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
