# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble the tracked external FR30 image set from generated image files."""

import argparse
import hashlib
import json
from pathlib import Path


EXPECTED_SIZE = 3428
EXPECTED_SHA256 = "8a7a5cac3cc6b6309020f3db0dc086dcec387e5fa2af1185b1a4b6c8455fd506"


def load_image(root: Path, manifest_name: str, role: str, filename: str,
               offset: int) -> tuple[bytes, dict[str, object]]:
    manifest = json.loads((root / manifest_name).read_text(encoding="utf-8"))
    if manifest_name == "widen-manifest.json":
        source = manifest["source"]
        source_sha256 = manifest["source_sha256"]
        image = next(item for item in manifest["images"] if item["bank"] == 0)
        linker = image["ld_script"]
        linker_sha256 = image["ld_sha256"]
        record = {
            "role": role,
            "file": filename,
            "offset": offset,
            "length": image["image_bytes"],
            "sha256": image["bin_sha256"],
            "base": image["base"],
            "entry_read": image["entry_read"],
            "entry_write": image["entry_write"],
            "build_id": image["build_id"],
            "capabilities": image["capabilities"],
            "source": source,
            "source_sha256": source_sha256,
            "linker": linker,
            "linker_sha256": linker_sha256,
            "elf_file": image["elf_file"],
            "elf_sha256": image["elf_sha256"],
        }
    else:
        source = manifest["source"]
        source_sha256 = manifest["source_sha256"]
        linker = manifest["linker"]
        linker_sha256 = manifest["linker_sha256"]
        record = {
            "role": role,
            "file": filename,
            "offset": offset,
            "length": manifest["image_bytes"],
            "sha256": manifest["bin_sha256"],
            "base": manifest["base"],
            "entry": manifest["entry"],
            "build_id": manifest["build_id"],
            "source": source,
            "source_sha256": source_sha256,
            "linker": linker,
            "linker_sha256": linker_sha256,
            "elf_file": manifest["elf_file"],
            "elf_sha256": manifest["elf_sha256"],
        }

    data = (root / filename).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != record["length"] or digest != record["sha256"]:
        raise ValueError(f"{filename}: generated image does not match its manifest")
    return data, record


def metadata_header(images: list[dict[str, object]], total: bytes) -> str:
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-or-later */",
        "/* Generated FR30 firmware metadata. Do not edit. */",
        "#ifndef TORNE_FR30_FIRMWARE_METADATA_H",
        "#define TORNE_FR30_FIRMWARE_METADATA_H",
        "",
        "#define TORNE_FR30_FIRMWARE_LAYOUT_VERSION 1U",
        f"#define TORNE_FR30_FIRMWARE_BYTES {len(total)}U",
        f'#define TORNE_FR30_FIRMWARE_SHA256 "{hashlib.sha256(total).hexdigest()}"',
    ]
    for image in images:
        prefix = "TORNE_FR30_" + str(image["role"]).upper().replace("-", "_")
        lines.extend([
            f"#define {prefix}_OFFSET {image['offset']}U",
            f"#define {prefix}_BYTES {image['length']}U",
            f'#define {prefix}_SHA256 "{image["sha256"]}"',
            f"#define {prefix}_BASE {image['base']}U",
            f"#define {prefix}_BUILD_ID {image['build_id']}U",
        ])
        if "entry_read" in image:
            lines.extend([
                f"#define {prefix}_ENTRY_READ {image['entry_read']}U",
                f"#define {prefix}_ENTRY_WRITE {image['entry_write']}U",
            ])
        else:
            lines.append(f"#define {prefix}_ENTRY {image['entry']}U")
    lines.extend(["", "#endif", ""])
    return "\n".join(lines)


def write_image_headers(source: Path, output: Path) -> None:
    widen = json.loads((source / "widen-manifest.json").read_text(encoding="utf-8"))
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-or-later */",
        "/* Generated FR30 firmware metadata. Do not edit. */",
        "#ifndef TORNE_WIDEN_IMAGES_H",
        "#define TORNE_WIDEN_IMAGES_H",
        "#include <stdint.h>",
        "",
    ]
    for image in widen["images"]:
        prefix = f"TORNE_WIDEN_BANK{image['bank']}"
        lines.extend([
            f"#define {prefix}_BASE {image['base']}u",
            f"#define {prefix}_IMAGE_BYTES {image['image_bytes']}u",
            f"#define {prefix}_ENTRY_READ {image['entry_read']}u",
            f"#define {prefix}_ENTRY_WRITE {image['entry_write']}u",
            f"#define {prefix}_BUILD_ID {image['build_id']}u",
            f'#define {prefix}_SHA256 "{image["bin_sha256"]}"',
        ])
    lines.extend([
        f'#define TORNE_WIDEN_SOURCE_SHA256 "{widen["source_sha256"]}"',
        "",
        "#endif",
        "",
    ])
    (output / "widen-images.h").write_text("\n".join(lines), encoding="utf-8")

    for core in ("core0", "core1"):
        manifest = json.loads(
            (source / f"{core}-manifest.json").read_text(encoding="utf-8")
        )
        prefix = f"TORNE_{core.upper()}"
        lines = [
            "/* SPDX-License-Identifier: GPL-2.0-or-later */",
            "/* Generated FR30 firmware metadata. Do not edit. */",
            f"#ifndef TORNE_{core.upper()}_IMAGES_H",
            f"#define TORNE_{core.upper()}_IMAGES_H",
            "#include <stdint.h>",
            "",
            f"#define {prefix}_BASE {manifest['base']}u",
            f"#define {prefix}_ENTRY {manifest['entry']}u",
            f"#define {prefix}_IMAGE_BYTES {manifest['image_bytes']}u",
            f"#define {prefix}_BUILD_ID {manifest['build_id']}u",
            f'#define {prefix}_SHA256 "{manifest["bin_sha256"]}"',
            "",
            "#endif",
            "",
        ]
        (output / f"{core}-images.h").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    source = args.input_dir
    images = []
    data = []
    offset = 0
    for role, manifest, filename in (
        ("widen-bank0", "widen-manifest.json", "widen-bank0.bin"),
        ("core0", "core0-manifest.json", "core0.bin"),
        ("core1", "core1-manifest.json", "core1.bin"),
    ):
        image_data, record = load_image(source, manifest, role, filename, offset)
        images.append(record)
        data.append(image_data)
        offset += len(image_data)

    bank1 = json.loads((source / "widen-manifest.json").read_text(encoding="utf-8"))
    image1 = next(item for item in bank1["images"] if item["bank"] == 1)
    bank1_data = (source / image1["bin_file"]).read_bytes()
    if len(bank1_data) != image1["image_bytes"] or hashlib.sha256(bank1_data).hexdigest() != image1["bin_sha256"]:
        raise ValueError("widen-bank1.bin does not match its manifest")

    asset = b"".join(data)
    digest = hashlib.sha256(asset).hexdigest()
    if len(asset) != EXPECTED_SIZE or digest != EXPECTED_SHA256:
        raise ValueError("FR30 image set differs from the fixed v1 asset")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "torne-fr30-v1.bin").write_bytes(asset)
    manifest = {
        "format": "torne-fr30-image-set-v1",
        "license": "GPL-2.0-or-later",
        "layout_version": 1,
        "file": "torne-fr30-v1.bin",
        "total_bytes": len(asset),
        "sha256": digest,
        "images": images,
    }
    (args.output_dir / "torne-fr30-v1-manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    (args.output_dir / "torne-fr30-v1-metadata.h").write_text(
        metadata_header(images, asset), encoding="utf-8"
    )
    write_image_headers(source, args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
