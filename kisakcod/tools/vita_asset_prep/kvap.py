#!/usr/bin/env python3
"""Deterministic KVAP package writer/inspector.

KVAP is deliberately a boring, fixed-width container.  It is the first
portable asset checkpoint; runtime asset schemas will be added as typed chunks
instead of dumping native C++ structures or fastfile pointers.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib
from pathlib import Path
from typing import Any

MAGIC = b"KVAP"
MAJOR = 1
MINOR = 0
ENDIAN_LITTLE = 1
HEADER_SIZE = 64
TOC_SIZE = 64
ALIGNMENT = 16

# Header: magic, major, minor, endian, flags, header size, toc offset,
# toc count, package id, source manifest hash.
HEADER = struct.Struct("<4sHHBBHQIQ32s")
# Chunk: type, flags, asset id, file offset, stored size, raw size, alignment,
# codec, CRC32, reserved.
TOC = struct.Struct("<IIQQQQIII12s")

CHUNK_TYPES = {
    "manifest": 1,
    "strings": 2,
    "collision": 3,
    "world": 4,
    "material": 5,
    "image": 6,
    "model": 7,
    "animation": 8,
    "weapon": 9,
    "script": 10,
    "menu": 11,
    "font": 12,
    "sound": 13,
    "hud": 14,
    "thumbnail": 15,
    "environment": 16,
    "primary_lights": 17,
    "effects": 18,
}
CODECS = {"none": 0, "zlib": 1}


class KvapError(RuntimeError):
    pass


def _align(value: int, alignment: int = ALIGNMENT) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def _asset_id(value: Any) -> int:
    if isinstance(value, int):
        return value & 0xFFFFFFFFFFFFFFFF
    text = str(value)
    if text.lower().startswith("0x"):
        return int(text, 16) & 0xFFFFFFFFFFFFFFFF
    if text.isdigit():
        return int(text) & 0xFFFFFFFFFFFFFFFF
    return int.from_bytes(hashlib.sha256(text.encode("utf-8")).digest()[:8], "little")


def _chunk_type(value: Any) -> int:
    if isinstance(value, int):
        return value
    text = str(value).lower()
    if text not in CHUNK_TYPES:
        raise KvapError(f"unknown chunk type: {value}")
    return CHUNK_TYPES[text]


def _codec(value: Any) -> int:
    if value is None:
        return 0
    if isinstance(value, int):
        return value
    text = str(value).lower()
    if text not in CODECS:
        raise KvapError(f"unknown codec: {value}")
    return CODECS[text]


def _read_source(path: Path, codec: int) -> tuple[bytes, bytes]:
    raw = path.read_bytes()
    if codec == CODECS["none"]:
        return raw, raw
    if codec == CODECS["zlib"]:
        return zlib.compress(raw, level=9), raw
    raise KvapError(f"unsupported codec {codec} for {path}")


def _hex_hash(value: Any) -> bytes:
    if not value:
        return bytes(32)
    text = str(value).lower().removeprefix("sha256:")
    try:
        result = bytes.fromhex(text)
    except ValueError as exc:
        raise KvapError("sourceHash must be a SHA-256 hex string") from exc
    if len(result) != 32:
        raise KvapError("sourceHash must contain exactly 32 bytes")
    return result


def create_package(manifest_path: Path, output_path: Path, workspace: Path, cod4_root: Path) -> dict[str, Any]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    chunks = manifest.get("chunks")
    if not isinstance(chunks, list) or not chunks:
        raise KvapError("manifest must contain a non-empty chunks array")

    output_path = output_path.resolve()
    workspace = workspace.resolve()
    cod4_root = cod4_root.resolve()
    if not _is_within(output_path, workspace) or _is_within(output_path, cod4_root):
        raise KvapError(f"output must be inside workspace and outside COD4 root: {output_path}")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    prepared: list[tuple[int, int, int, Path, bytes, bytes]] = []
    for item in chunks:
        if not isinstance(item, dict) or "path" not in item:
            raise KvapError("each chunk needs type, assetId, and path")
        source = Path(str(item["path"])).resolve()
        if not source.is_file():
            raise KvapError(f"chunk source does not exist: {source}")
        if not (_is_within(source, workspace) or _is_within(source, cod4_root)):
            raise KvapError(f"chunk source is outside approved roots: {source}")
        kind = _chunk_type(item.get("type"))
        asset = _asset_id(item.get("assetId", source.name))
        codec = _codec(item.get("codec"))
        stored, raw = _read_source(source, codec)
        prepared.append((kind, asset, codec, source, stored, raw))

    # Stable ordering keeps package hashes reproducible across clean runs.
    prepared.sort(key=lambda entry: (entry[0], entry[1], entry[3].as_posix().lower()))
    source_hash = _hex_hash(manifest.get("sourceHash"))
    digest = hashlib.sha256()
    for kind, asset, codec, source, stored, raw in prepared:
        digest.update(struct.pack("<IQQI", kind, asset, len(raw), codec))
        digest.update(hashlib.sha256(raw).digest())
    package_id = int.from_bytes(digest.digest()[:8], "little")

    toc_offset = HEADER_SIZE
    data_offset = _align(toc_offset + TOC_SIZE * len(prepared))
    toc_records: list[bytes] = []
    payloads: list[tuple[int, bytes]] = []
    cursor = data_offset
    report_chunks: list[dict[str, Any]] = []
    for kind, asset, codec, source, stored, raw in prepared:
        cursor = _align(cursor)
        crc = zlib.crc32(raw) & 0xFFFFFFFF
        toc_records.append(TOC.pack(kind, 0, asset, cursor, len(stored), len(raw), ALIGNMENT, codec, crc, bytes(12)))
        payloads.append((cursor, stored))
        report_chunks.append({
            "type": kind,
            "assetId": f"0x{asset:016x}",
            "source": str(source),
            "offset": cursor,
            "storedBytes": len(stored),
            "rawBytes": len(raw),
            "codec": codec,
            "crc32": f"{crc:08x}",
        })
        cursor += len(stored)

    header = HEADER.pack(MAGIC, MAJOR, MINOR, ENDIAN_LITTLE, 0, HEADER_SIZE,
                         toc_offset, len(toc_records), package_id, source_hash)
    with output_path.open("wb") as stream:
        stream.write(header)
        for record in toc_records:
            stream.write(record)
        if stream.tell() < data_offset:
            stream.write(bytes(data_offset - stream.tell()))
        for offset, payload in payloads:
            if stream.tell() < offset:
                stream.write(bytes(offset - stream.tell()))
            stream.write(payload)

    report = {
        "format": "KVAP",
        "version": f"{MAJOR}.{MINOR}",
        "packageId": f"0x{package_id:016x}",
        "sourceHash": source_hash.hex(),
        "output": str(output_path),
        "bytes": output_path.stat().st_size,
        "chunks": report_chunks,
    }
    report_path = output_path.with_suffix(output_path.suffix + ".report.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def inspect_package(path: Path) -> dict[str, Any]:
    with path.open("rb") as stream:
        header_bytes = stream.read(HEADER_SIZE)
        if len(header_bytes) != HEADER_SIZE:
            raise KvapError("package is shorter than the fixed header")
        magic, major, minor, endian, flags, header_size, toc_offset, toc_count, package_id, source_hash = HEADER.unpack(header_bytes)
        if magic != MAGIC:
            raise KvapError(f"bad magic: {magic!r}")
        if endian != ENDIAN_LITTLE or header_size != HEADER_SIZE:
            raise KvapError("unsupported KVAP header")
        stream.seek(toc_offset)
        chunks = []
        for index in range(toc_count):
            record = stream.read(TOC_SIZE)
            if len(record) != TOC_SIZE:
                raise KvapError(f"truncated TOC record {index}")
            kind, chunk_flags, asset, offset, stored, raw, alignment, codec, crc, _ = TOC.unpack(record)
            chunks.append({"index": index, "type": kind, "assetId": f"0x{asset:016x}",
                           "offset": offset, "storedBytes": stored, "rawBytes": raw,
                           "alignment": alignment, "codec": codec, "crc32": f"{crc:08x}"})
    return {"format": "KVAP", "version": f"{major}.{minor}", "flags": flags,
            "packageId": f"0x{package_id:016x}", "sourceHash": source_hash.hex(),
            "bytes": path.stat().st_size, "chunks": chunks}


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="create a deterministic package from a JSON manifest")
    create.add_argument("--manifest", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--workspace", type=Path, required=True)
    create.add_argument("--cod4-root", type=Path, required=True)
    inspect = sub.add_parser("inspect", help="inspect a package without extracting it")
    inspect.add_argument("package", type=Path)
    args = parser.parse_args(argv)
    try:
        result = (create_package(args.manifest, args.output, args.workspace, args.cod4_root)
                  if args.command == "create" else inspect_package(args.package))
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, KvapError, json.JSONDecodeError) as exc:
        print(f"kvap: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
