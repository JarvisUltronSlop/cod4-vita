#!/usr/bin/env python3
"""Report the one-client server-static footprint from checked-in source sizes."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


PROFILE = {
    "KISAK_SVS_CACHED_SNAPSHOT_FRAMES": 64,
    "KISAK_SVS_CACHED_SNAPSHOT_ENTITIES": 2048,
    "KISAK_SVS_MAX_CLIENTS": 1,
    "KISAK_SVS_SNAPSHOT_ENTITIES": 4096,
    "KISAK_SVS_SNAPSHOT_CLIENTS": 2048,
    "KISAK_SVS_ARCHIVED_SNAPSHOT_FRAMES": 64,
    "KISAK_SVS_ARCHIVED_SNAPSHOT_BUFFER": 0x400000,
    "KISAK_SVS_CACHED_SNAPSHOT_CLIENTS": 64,
    "KISAK_SVS_CHALLENGES": 8,
    "KISAK_SVS_TEMP_BANS": 1,
}

ARRAYS = {
    "cachedSnapshotFrames": ("cachedSnapshot_t", "KISAK_SVS_CACHED_SNAPSHOT_FRAMES"),
    "cachedSnapshotEntities": ("archivedEntity_s", "KISAK_SVS_CACHED_SNAPSHOT_ENTITIES"),
    "clients": ("client_t", "KISAK_SVS_MAX_CLIENTS"),
    "snapshotEntities": ("entityState_s", "KISAK_SVS_SNAPSHOT_ENTITIES"),
    "snapshotClients": ("clientState_s", "KISAK_SVS_SNAPSHOT_CLIENTS"),
    "archivedSnapshotFrames": ("archivedSnapshot_s", "KISAK_SVS_ARCHIVED_SNAPSHOT_FRAMES"),
    "cachedSnapshotClients": ("cachedClient_s", "KISAK_SVS_CACHED_SNAPSHOT_CLIENTS"),
    "challenges": ("challenge_t", "KISAK_SVS_CHALLENGES"),
    "tempBans": ("tempBanSlot_t", "KISAK_SVS_TEMP_BANS"),
}


def sizes_from_source(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    result: dict[str, int] = {}
    for name in set(type_name for type_name, _ in ARRAYS.values()):
        match = re.search(rf"struct\s+{re.escape(name)}\s*//\s*sizeof=0x([0-9A-Fa-f]+)", text)
        if match:
            result[name] = int(match.group(1), 16)
    # entityState_s and clientState_s are declared in adjacent shared headers.
    for header in (path.parent.parent / "qcommon" / "ent.h", path.parent.parent / "client_mp" / "client_mp.h"):
        if not header.is_file():
            continue
        text = header.read_text(encoding="utf-8")
        for name in ("entityState_s", "clientState_s", "archivedEntity_s"):
            match = re.search(rf"struct\s+{re.escape(name)}\s*//\s*sizeof=0x([0-9A-Fa-f]+)", text)
            if match:
                result[name] = int(match.group(1), 16)
    missing = sorted(set(type_name for type_name, _ in ARRAYS.values()) - result.keys())
    if missing:
        raise RuntimeError("missing source size comments: " + ", ".join(missing))
    return result


def report(header: Path) -> dict:
    sizes = sizes_from_source(header)
    rows = []
    total = 0
    for field, (type_name, macro) in ARRAYS.items():
        count = PROFILE[macro]
        size = sizes[type_name]
        bytes_used = count * size
        total += bytes_used
        rows.append({"field": field, "type": type_name, "count": count, "elementBytes": size, "bytes": bytes_used})
    total += PROFILE["KISAK_SVS_ARCHIVED_SNAPSHOT_BUFFER"]
    rows.append({"field": "archivedSnapshotBuffer", "type": "byte", "count": PROFILE["KISAK_SVS_ARCHIVED_SNAPSHOT_BUFFER"], "elementBytes": 1, "bytes": PROFILE["KISAK_SVS_ARCHIVED_SNAPSHOT_BUFFER"]})
    return {"profile": PROFILE, "estimatedServerStaticBytes": total,
            "estimatedServerStaticMiB": round(total / 1048576, 3), "rows": rows,
            "passesEightMiBGate": total < 8 * 1048576}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, default=Path(__file__).parents[2] / "src/server_mp/server_mp.h")
    args = parser.parse_args()
    result = report(args.header)
    print(json.dumps(result, indent=2))
    return 0 if result["passesEightMiBGate"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
