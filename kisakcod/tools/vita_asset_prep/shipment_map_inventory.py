#!/usr/bin/env python3
"""Inventory high-confidence Shipment asset-name hints from a retail map fastfile.

The IWffu100 payload is an x86 database image.  This tool deliberately does not
interpret pointers or emit runtime assets.  It only records printable names that
match known Call of Duty asset namespaces, so the eventual Windows DB exporter
has a bounded, reviewable starting point.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Iterable

from fastfile import decompress


_PRINTABLE_RUN = re.compile(rb"[\x20-\x7e]{4,128}")
_SAFE_NAME = re.compile(r"^[A-Za-z0-9_$./-]+$")
# Dollar-prefixed image aliases are common in UI data, but the opaque map
# stream also contains many short `$A1l`-style binary fragments.  Require a
# descriptive lowercase run to keep those fragments out of the inventory.
_DOLLAR_NAME = re.compile(r"^\$[A-Za-z0-9_]*[a-z]{3,}[A-Za-z0-9_]*$")


def _classify(name: str) -> str | None:
    """Return a conservative semantic hint for one printable name."""

    lower = name.lower()
    if name.startswith("mc/mtl_") or name.startswith("mc/ch_") or name.startswith("mc/me_"):
        return "material"
    if name.startswith(("xmodel/", "model/", "mc/model_")) or lower.endswith((".xmodel", ".xmodel_bin")):
        return "model"
    if name.startswith(("xanim/", "anim/")) or lower.endswith((".xanim", ".xanim_bin")):
        return "animation"
    if name.startswith(("maps/", "map/")) or lower.endswith((".d3dbsp", ".d3dprt")):
        return "world"
    if name.startswith(("sound/", "snd/", "soundaliases/")) or lower.endswith((".wav", ".mp3")):
        return "sound"
    if name.startswith(("weapons/", "weapon_")):
        return "weapon"
    if name.startswith(("fx/", "effects/")):
        return "effect"
    if name.startswith(("ui/", "menu/")) or lower.endswith((".menu", ".gfx")):
        return "menu"
    if _DOLLAR_NAME.fullmatch(name):
        return "image"
    return None


def extract_references(payload: bytes) -> list[dict[str, str]]:
    """Extract stable, deduplicated reference records from an opaque payload."""

    seen: set[tuple[str, str]] = set()
    records: list[dict[str, str]] = []
    for raw in _PRINTABLE_RUN.findall(payload):
        name = raw.decode("ascii", "ignore").strip()
        if not _SAFE_NAME.fullmatch(name):
            continue
        kind = _classify(name)
        if kind is None or (kind == "image" and len(name) > 72):
            continue
        key = (kind, name)
        if key in seen:
            continue
        seen.add(key)
        records.append({"kind": kind, "name": name})
    records.sort(key=lambda record: (record["kind"], record["name"]))
    return records


def inventory(source: Path) -> dict[str, object]:
    envelope, payload = decompress(source)
    source_meta = dict(envelope)
    source_meta["source"] = source.name
    source_meta["relativeSource"] = f"zone/english/{source.name}"
    source_meta["status"] = "opaque-string-hints-not-runtime-assets"
    references = extract_references(payload)
    counts: dict[str, int] = {}
    for record in references:
        counts[record["kind"]] = counts.get(record["kind"], 0) + 1
    return {
        "format": "KisakCOD Vita opaque map string inventory",
        "version": 1,
        "source": source_meta,
        "counts": dict(sorted(counts.items())),
        "references": references,
        "status": "reference-only-pending-typed-db-export",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    try:
        workspace = args.workspace.resolve()
        output = args.output.resolve()
        output.relative_to(workspace)
        document = inventory(args.source.resolve())
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({
            "output": str(output),
            "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "references": len(document["references"]),
            "counts": document["counts"],
            "status": document["status"],
        }, indent=2))
        return 0
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
