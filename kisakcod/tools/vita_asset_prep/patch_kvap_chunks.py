#!/usr/bin/env python3
"""Replace or append selected raw chunks in an existing KVAP.

This is for qualification builds where one newly generated typed sidecar must
be tested against an otherwise complete, already validated multi-map package.
All unselected chunks are copied byte-for-byte; selected replacements are
written uncompressed and receive fresh size, CRC, and package-id metadata.
New chunks use the same deterministic uncompressed representation and are
appended to the TOC without requiring the original source manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path
from typing import Any

from kvap import (ALIGNMENT, CHUNK_TYPES, ENDIAN_LITTLE, HEADER, HEADER_SIZE,
                  MAGIC, TOC, TOC_SIZE)


def _align(value: int, alignment: int = ALIGNMENT) -> int:
    alignment = max(1, int(alignment))
    return (value + alignment - 1) // alignment * alignment


def _asset_id(value: str) -> int:
    text = value.strip()
    if text.lower().startswith("0x"):
        return int(text, 16) & 0xFFFFFFFFFFFFFFFF
    return int(text, 10) & 0xFFFFFFFFFFFFFFFF


def _chunk_type(value: str) -> int:
    text = value.strip().lower()
    if text in CHUNK_TYPES:
        return CHUNK_TYPES[text]
    return int(text, 0)


def _read_package(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        header_bytes = stream.read(HEADER_SIZE)
        if len(header_bytes) != HEADER_SIZE:
            raise ValueError("package header is truncated")
        (magic, major, minor, endian, flags, header_size, toc_offset,
         toc_count, package_id, source_hash) = HEADER.unpack(header_bytes)
        if magic != MAGIC or endian != ENDIAN_LITTLE or header_size != HEADER_SIZE:
            raise ValueError("unsupported KVAP package header")
        if toc_count == 0 or toc_count > 4096:
            raise ValueError("invalid KVAP chunk count")
        stream.seek(toc_offset)
        chunks: list[dict[str, Any]] = []
        for index in range(toc_count):
            record = stream.read(TOC_SIZE)
            if len(record) != TOC_SIZE:
                raise ValueError(f"TOC record {index} is truncated")
            (kind, chunk_flags, asset_id, offset, stored_size, raw_size,
             alignment, codec, crc32, reserved) = TOC.unpack(record)
            if alignment == 0 or offset + stored_size > file_size:
                raise ValueError(f"TOC record {index} is out of bounds")
            if codec not in (0, 1):
                raise ValueError(f"unsupported codec {codec} in chunk {index}")
            chunks.append({
                "index": index,
                "type": kind,
                "chunkFlags": chunk_flags,
                "assetId": asset_id,
                "sourceOffset": offset,
                "offset": offset,
                "storedSize": stored_size,
                "rawSize": raw_size,
                "alignment": alignment,
                "codec": codec,
                "crc32": crc32,
                "reserved": reserved,
            })
    header = {
        "major": major,
        "minor": minor,
        "flags": flags,
        "sourceHash": source_hash,
        "packageId": package_id,
    }
    return header, chunks


def _hash_file(path: Path) -> tuple[int, int, bytes]:
    digest = hashlib.sha256()
    crc32 = 0
    size = 0
    with path.open("rb") as stream:
        while True:
            block = stream.read(4 * 1024 * 1024)
            if not block:
                break
            digest.update(block)
            crc32 = zlib.crc32(block, crc32)
            size += len(block)
    return size, crc32 & 0xFFFFFFFF, digest.digest()


def _hash_package_chunk(path: Path, chunk: dict[str, Any]) -> tuple[int, int, bytes]:
    digest = hashlib.sha256()
    crc32 = 0
    raw_size = 0
    with path.open("rb") as stream:
        stream.seek(chunk["sourceOffset"])
        remaining = chunk["storedSize"]
        decompressor = zlib.decompressobj() if chunk["codec"] == 1 else None
        while remaining:
            block = stream.read(min(4 * 1024 * 1024, remaining))
            if not block:
                raise ValueError(f"chunk {chunk['index']} is truncated")
            remaining -= len(block)
            raw_block = (decompressor.decompress(block)
                         if decompressor is not None else block)
            if raw_block:
                digest.update(raw_block)
                crc32 = zlib.crc32(raw_block, crc32)
                raw_size += len(raw_block)
        if decompressor is not None:
            raw_block = decompressor.flush()
            if raw_block:
                digest.update(raw_block)
                crc32 = zlib.crc32(raw_block, crc32)
                raw_size += len(raw_block)
    return raw_size, crc32 & 0xFFFFFFFF, digest.digest()


def _copy_package_chunk(source, chunk: dict[str, Any], destination) -> None:
    source.seek(chunk["sourceOffset"])
    remaining = chunk["storedSize"]
    while remaining:
        block = source.read(min(4 * 1024 * 1024, remaining))
        if not block:
            raise ValueError(f"chunk {chunk['index']} is truncated while copying")
        destination.write(block)
        remaining -= len(block)


def _copy_file(path: Path, destination) -> None:
    with path.open("rb") as source:
        while True:
            block = source.read(4 * 1024 * 1024)
            if not block:
                break
            destination.write(block)


def patch_package(package_path: Path, output_path: Path,
                  replacements: list[tuple[str, str, Path]],
                  additions: list[tuple[str, str, Path]] | None = None
                  ) -> dict[str, Any]:
    package_path = package_path.resolve()
    output_path = output_path.resolve()
    if package_path == output_path:
        raise ValueError("output must be a new package path")
    if output_path.exists():
        raise ValueError(f"refusing to overwrite existing output: {output_path}")

    header, chunks = _read_package(package_path)
    replacement_paths: dict[tuple[int, int], Path] = {}
    for kind_text, asset_text, source_text in replacements:
        key = (_chunk_type(kind_text), _asset_id(asset_text))
        if key in replacement_paths:
            raise ValueError(f"duplicate replacement target: {kind_text} {asset_text}")
        source = Path(source_text).resolve()
        if not source.is_file():
            raise ValueError(f"replacement source does not exist: {source}")
        replacement_paths[key] = source

    addition_paths: dict[tuple[int, int], Path] = {}
    for kind_text, asset_text, source_text in additions or []:
        key = (_chunk_type(kind_text), _asset_id(asset_text))
        if key in replacement_paths or key in addition_paths:
            raise ValueError(f"duplicate patch target: {kind_text} {asset_text}")
        source = Path(source_text).resolve()
        if not source.is_file():
            raise ValueError(f"addition source does not exist: {source}")
        addition_paths[key] = source

    matched: set[tuple[int, int]] = set()
    for chunk in chunks:
        key = (chunk["type"], chunk["assetId"])
        replacement = replacement_paths.get(key)
        if replacement is None:
            continue
        chunk["replacementPath"] = replacement
        chunk["codec"] = 0
        chunk["storedSize"] = replacement.stat().st_size
        chunk["rawSize"] = chunk["storedSize"]
        _, chunk["crc32"], chunk["rawDigest"] = _hash_file(replacement)
        matched.add(key)
    missing = sorted(set(replacement_paths) - matched)
    if missing:
        pretty = ", ".join(f"{kind}:{asset:#x}" for kind, asset in missing)
        raise ValueError(f"replacement target is missing from package: {pretty}")

    existing_keys = {(chunk["type"], chunk["assetId"]) for chunk in chunks}
    duplicate_additions = sorted(set(addition_paths) & existing_keys)
    if duplicate_additions:
        pretty = ", ".join(f"{kind}:{asset:#x}"
                           for kind, asset in duplicate_additions)
        raise ValueError(f"addition target already exists in package: {pretty}")
    for key, source in addition_paths.items():
        raw_size, crc32, raw_digest = _hash_file(source)
        kind, asset_id = key
        chunks.append({
            "index": len(chunks),
            "type": kind,
            "chunkFlags": 0,
            "assetId": asset_id,
            "sourceOffset": 0,
            "offset": 0,
            "storedSize": raw_size,
            "rawSize": raw_size,
            "alignment": ALIGNMENT,
            "codec": 0,
            "crc32": crc32,
            "reserved": bytes(12),
            "replacementPath": source,
            "rawDigest": raw_digest,
        })
    if len(chunks) > 4096:
        raise ValueError("patched package would exceed the TOC limit")

    digest = hashlib.sha256()
    for chunk in sorted(chunks,
                        key=lambda item: (item["type"], item["assetId"],
                                          item["index"])):
        replacement = chunk.get("replacementPath")
        if replacement is not None:
            raw_size, crc32, raw_digest = _hash_file(replacement)
        else:
            raw_size, crc32, raw_digest = _hash_package_chunk(package_path, chunk)
        if raw_size != chunk["rawSize"] or crc32 != chunk["crc32"]:
            raise ValueError(f"chunk {chunk['index']} payload does not match TOC")
        digest.update(struct.pack("<IQQI", chunk["type"], chunk["assetId"],
                                  raw_size, chunk["codec"]))
        digest.update(raw_digest)
    package_id = int.from_bytes(digest.digest()[:8], "little")

    cursor = _align(HEADER_SIZE + TOC_SIZE * len(chunks))
    for chunk in chunks:
        cursor = _align(cursor, chunk["alignment"])
        chunk["offset"] = cursor
        cursor += chunk["storedSize"]

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with package_path.open("rb") as source, output_path.open("wb") as destination:
        destination.write(HEADER.pack(
            MAGIC, header["major"], header["minor"], ENDIAN_LITTLE,
            header["flags"], HEADER_SIZE, HEADER_SIZE, len(chunks),
            package_id, header["sourceHash"]))
        for chunk in chunks:
            destination.write(TOC.pack(
                chunk["type"], chunk["chunkFlags"], chunk["assetId"],
                chunk["offset"], chunk["storedSize"], chunk["rawSize"],
                chunk["alignment"], chunk["codec"], chunk["crc32"],
                chunk["reserved"]))
        if destination.tell() < chunks[0]["offset"]:
            destination.write(bytes(chunks[0]["offset"] - destination.tell()))
        for chunk in chunks:
            if destination.tell() < chunk["offset"]:
                destination.write(bytes(chunk["offset"] - destination.tell()))
            replacement = chunk.get("replacementPath")
            if replacement is not None:
                _copy_file(replacement, destination)
            else:
                _copy_package_chunk(source, chunk, destination)

    report = {
        "format": "KVAP",
        "version": f"{header['major']}.{header['minor']}",
        "packageId": f"0x{package_id:016x}",
        "sourceHash": header["sourceHash"].hex(),
        "input": str(package_path),
        "output": str(output_path),
        "bytes": output_path.stat().st_size,
        "replacements": [{
            "type": kind,
            "assetId": f"0x{_asset_id(asset):016x}",
            "source": str(Path(source).resolve()),
            "bytes": replacement_paths[(_chunk_type(kind), _asset_id(asset))].stat().st_size,
        } for kind, asset, source in replacements],
        "additions": [{
            "type": kind,
            "assetId": f"0x{_asset_id(asset):016x}",
            "source": str(Path(source).resolve()),
            "bytes": addition_paths[(_chunk_type(kind), _asset_id(asset))].stat().st_size,
        } for kind, asset, source in additions or []],
        "chunks": [{
            "index": chunk["index"],
            "type": chunk["type"],
            "assetId": f"0x{chunk['assetId']:016x}",
            "offset": chunk["offset"],
            "storedBytes": chunk["storedSize"],
            "rawBytes": chunk["rawSize"],
            "codec": chunk["codec"],
            "crc32": f"{chunk['crc32']:08x}",
        } for chunk in chunks],
    }
    output_path.with_suffix(output_path.suffix + ".report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--replace", nargs=3, action="append",
                        metavar=("TYPE", "ASSET_ID", "SOURCE"),
                        help="replace one chunk; TYPE may be a KVAP name")
    parser.add_argument("--add", nargs=3, action="append",
                        metavar=("TYPE", "ASSET_ID", "SOURCE"),
                        help="append one new uncompressed chunk")
    args = parser.parse_args(argv)
    if not args.replace and not args.add:
        parser.error("at least one --replace or --add operation is required")
    try:
        report = patch_package(args.package, args.output,
                               args.replace or [], args.add or [])
    except (OSError, ValueError, struct.error, zlib.error) as exc:
        print(f"patch-kvap: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
