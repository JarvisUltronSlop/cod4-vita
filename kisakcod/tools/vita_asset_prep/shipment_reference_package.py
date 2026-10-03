#!/usr/bin/env python3
"""Build a deterministic, pointer-free Shipment reference package.

This is an intermediate asset gate.  It packages the names discovered by the
opaque map inventory and the typed load-string records, but it deliberately
does *not* pretend those names are world, collision, model, or texture
payloads.  The package is useful for exercising Vita registry lookup and for
reviewing the dependency closure while the Windows DB exporter is completed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvap import create_package  # noqa: E402
from shipment_load_convert import convert as convert_load  # noqa: E402
from shipment_map_inventory import inventory as inventory_map  # noqa: E402


_CHUNK_TYPES = {
    "world", "material", "image", "model", "animation", "weapon", "sound",
    "script", "menu", "font", "hud", "thumbnail",
}


def _write_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _source_hash(load_doc: dict[str, Any], map_doc: dict[str, Any]) -> str:
    digest = hashlib.sha256()
    for document in (load_doc, map_doc):
        source = document["source"]
        digest.update(str(source["sourceSha256"]).encode("ascii"))
        digest.update(str(source["rawSha256"]).encode("ascii"))
    return digest.hexdigest()


def build_reference_package(load_source: Path, map_source: Path,
                           workspace: Path, output: Path) -> dict[str, Any]:
    """Convert two read-only fastfiles into a local reference-only KVAP."""

    workspace = workspace.resolve()
    output = output.resolve()
    output.relative_to(workspace)
    load_source = load_source.resolve()
    map_source = map_source.resolve()
    if not load_source.is_file() or not map_source.is_file():
        raise FileNotFoundError("Shipment load/map fastfile is missing")

    load_doc = convert_load(load_source)
    map_doc = inventory_map(map_source)
    converted = workspace / "assets" / "converted" / "shipment-reference"
    manifest = {
        "format": "KisakCOD Vita Shipment reference package",
        "version": 1,
        "status": "reference-only-pending-typed-db-export",
        "playable": False,
        "recordSchema": "name-only-reference-v1",
        "source": {
            "load": load_doc["source"],
            "map": map_doc["source"],
        },
        "counts": map_doc["counts"],
    }
    manifest_path = converted / "manifest.json"
    _write_json(manifest_path, manifest)

    strings_doc = {
        "format": "KisakCOD Vita typed string records",
        "version": 1,
        "source": load_doc["source"],
        "strings": load_doc["assetTypes"]["strings"],
        "status": "typed-string-records",
    }
    strings_path = converted / "strings.json"
    _write_json(strings_path, strings_doc)

    chunks: list[dict[str, str]] = [
        {"type": "manifest", "assetId": "shipment.reference.manifest", "path": str(manifest_path)},
        {"type": "strings", "assetId": "shipment.reference.strings", "path": str(strings_path)},
    ]
    by_kind: dict[str, list[dict[str, str]]] = {}
    for reference in map_doc["references"]:
        kind = str(reference["kind"])
        if kind in _CHUNK_TYPES:
            by_kind.setdefault(kind, []).append({
                "name": str(reference["name"]),
                "payloadStatus": "pending-typed-db-export",
            })

    for kind in sorted(by_kind):
        records_path = converted / f"{kind}.json"
        _write_json(records_path, {
            "format": "KisakCOD Vita reference records",
            "version": 1,
            "assetType": kind,
            "recordSchema": "name-only-reference-v1",
            "records": by_kind[kind],
            "status": "reference-only-pending-typed-db-export",
        })
        chunks.append({
            "type": kind,
            "assetId": f"shipment.reference.{kind}",
            "path": str(records_path),
        })

    package_manifest = {
        "sourceHash": _source_hash(load_doc, map_doc),
        "chunks": chunks,
        "status": manifest["status"],
    }
    package_manifest_path = converted / "package-manifest.json"
    _write_json(package_manifest_path, package_manifest)
    report = create_package(package_manifest_path, output, workspace, load_source.parents[2])
    report["status"] = manifest["status"]
    report["playable"] = False
    report["referenceCounts"] = map_doc["counts"]
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--load-fastfile", type=Path, required=True)
    parser.add_argument("--map-fastfile", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = build_reference_package(args.load_fastfile, args.map_fastfile,
                                         args.workspace, args.output)
        print(json.dumps(report, indent=2))
        return 0
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
