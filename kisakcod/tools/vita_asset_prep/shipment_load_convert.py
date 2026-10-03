#!/usr/bin/env python3
"""Convert the tiny Shipment load fastfile into pointer-free typed records."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fastfile import decompress


def _strings(payload: bytes) -> list[str]:
    result: list[str] = []
    current = bytearray()
    for value in payload:
        if 32 <= value < 127:
            current.append(value)
        else:
            if len(current) >= 4:
                result.append(current.decode("ascii"))
            current.clear()
    if len(current) >= 4:
        result.append(current.decode("ascii"))
    return list(dict.fromkeys(result))


def convert(source: Path) -> dict[str, object]:
    envelope, payload = decompress(source)
    package_source = dict(envelope)
    # Keep provenance useful on the Vita without embedding the developer's
    # Windows drive/path in the shipped package.
    package_source["source"] = source.name
    package_source["relativeSource"] = f"zone/english/{source.name}"
    names = _strings(payload)
    image_names = [name for name in names if name.startswith("$") or "backdrop" in name
                   or name.startswith("loadscreen_")]
    return {
        "format": "KisakCOD Vita typed load records",
        "version": 1,
        "source": package_source,
        "assetTypes": {
            "strings": [{"value": name} for name in names],
            "image": [{"name": name, "format": "DXT1"} for name in image_names],
        },
        "status": "typed-load-records",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--strings-output", type=Path)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    try:
        output = args.output.resolve()
        workspace = args.workspace.resolve()
        output.relative_to(workspace)
        converted = convert(args.source)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(converted, indent=2) + "\n", encoding="utf-8")
        if args.strings_output:
            strings_output = args.strings_output.resolve()
            strings_output.relative_to(workspace)
            strings_output.parent.mkdir(parents=True, exist_ok=True)
            strings_doc = {
                "format": "KisakCOD Vita typed string records",
                "version": 1,
                "source": converted["source"],
                "strings": converted["assetTypes"]["strings"],
                "status": "typed-string-records",
            }
            strings_output.write_text(json.dumps(strings_doc, indent=2) + "\n",
                                      encoding="utf-8")
        print(json.dumps({"output": str(output), "sha256": hashlib.sha256(
            output.read_bytes()).hexdigest(), "status": converted["status"]}, indent=2))
        return 0
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
