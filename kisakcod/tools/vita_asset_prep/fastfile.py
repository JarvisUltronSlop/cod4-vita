#!/usr/bin/env python3
"""Read the retail IWffu100 zlib envelope without mutating the install.

The decompressed stream is still an x86 database image and is *not* a Vita
asset.  This tool only supplies the safe staging step used by the later
Windows DB exporter; it never emits a KVAP chunk by itself.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path


HEADER = struct.Struct("<8sI")
MAGIC = b"IWffu100"


class FastfileError(RuntimeError):
    pass


def decompress(path: Path) -> tuple[dict[str, object], bytes]:
    path = path.resolve()
    raw = path.read_bytes()
    if len(raw) < HEADER.size:
        raise FastfileError(f"fastfile is shorter than the envelope: {path}")
    magic, version = HEADER.unpack_from(raw)
    if magic != MAGIC:
        raise FastfileError(f"unsupported fastfile magic: {magic!r}")
    try:
        payload = zlib.decompress(raw[HEADER.size:])
    except zlib.error as exc:
        raise FastfileError(f"zlib stream is corrupt: {path}") from exc
    report = {
        "source": str(path),
        "magic": magic.decode("ascii"),
        "version": version,
        "compressedBytes": len(raw) - HEADER.size,
        "rawBytes": len(payload),
        "sourceSha256": hashlib.sha256(raw).hexdigest(),
        "rawSha256": hashlib.sha256(payload).hexdigest(),
        "status": "decompressed-x86-db-pending-typed-export",
    }
    return report, payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fastfile", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--workspace", type=Path,
                        help="workspace root required when --output is used")
    args = parser.parse_args()
    try:
        report, payload = decompress(args.fastfile)
        if args.output:
            output = args.output.resolve()
            if not args.workspace:
                raise FastfileError("--workspace is required with --output")
            workspace = args.workspace.resolve()
            try:
                output.relative_to(workspace)
            except ValueError as exc:
                raise FastfileError(f"output must be inside workspace: {output}") from exc
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(payload)
            report["output"] = str(output)
        print(json.dumps(report, indent=2))
        return 0
    except (OSError, FastfileError) as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
