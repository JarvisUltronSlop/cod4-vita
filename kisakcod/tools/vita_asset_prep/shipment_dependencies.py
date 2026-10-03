#!/usr/bin/env python3
"""Record the read-only Shipment source closure before typed conversion.

This command is intentionally a provenance pass, not a fastfile parser.  It
hashes the exact retail inputs and records the typed KVAP kinds that the
Windows-side exporter must produce.  A raw ``.ff`` is never copied into a Vita
package as a world or collision object.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


REQUIRED = (
    {
        "path": Path("zone/english/mp_shipment_load.ff"),
        "assetId": "mp_shipment_load.ff",
        "sourceRole": "load-fastfile",
        "expectedTypes": ["manifest", "strings", "menu"],
    },
    {
        "path": Path("zone/english/mp_shipment.ff"),
        "assetId": "mp_shipment",
        "sourceRole": "map-fastfile",
        "expectedTypes": [
            "world",
            "collision",
            "material",
            "image",
            "model",
            "weapon",
            "sound",
            "hud",
        ],
    },
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_manifest(cod4_root: Path) -> dict[str, object]:
    cod4_root = cod4_root.resolve()
    entries: list[dict[str, object]] = []
    for requirement in REQUIRED:
        source = (cod4_root / requirement["path"]).resolve()
        try:
            source.relative_to(cod4_root)
        except ValueError as exc:
            raise ValueError(f"source escaped COD4 root: {source}") from exc
        if not source.is_file():
            raise FileNotFoundError(source)
        entries.append({
            "source": str(source),
            "relativeSource": requirement["path"].as_posix(),
            "assetId": requirement["assetId"],
            "sourceRole": requirement["sourceRole"],
            "bytes": source.stat().st_size,
            "sha256": _sha256(source),
            "expectedTypes": requirement["expectedTypes"],
            "conversion": "pending-typed-db-export",
        })
    return {
        "format": "COD4 Shipment dependency inventory",
        "sourceRoot": str(cod4_root),
        "readOnly": True,
        "status": "source-verified-conversion-pending",
        "entries": entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cod4-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = build_manifest(args.cod4_root)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
