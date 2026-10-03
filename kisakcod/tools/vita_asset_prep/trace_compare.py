#!/usr/bin/env python3
"""Compare bounded CODVIT Shipment JSONL traces from PC and Vita.

The Vita runtime writes a small header followed by one authoritative gameplay
sample per line.  This tool deliberately compares only scalar command/state
fields, so it can be used with a PC reference harness without importing any
desktop engine structures or copyrighted assets.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any


FORMAT = "CODVIT-SHIPMENT-TRACE-2"
REQUIRED = {
    "frame",
    "time",
    "forward",
    "right",
    "buttons",
    "angles",
    "origin",
    "velocity",
    "stance",
    "weapon",
    "clip",
    "reserve",
    "health",
    "ads",
    "firing",
    "reloading",
    "viewHeight",
    "nextFire",
    "reloadStart",
    "reloadEnd",
}


def _finite_vector(value: Any, length: int, label: str) -> list[float]:
    if not isinstance(value, list) or len(value) != length:
        raise ValueError(f"{label} must contain {length} numbers")
    result = [float(item) for item in value]
    if not all(math.isfinite(item) for item in result):
        raise ValueError(f"{label} contains a non-finite number")
    return result


def load_trace(path: Path, max_frames: int) -> list[dict[str, Any]]:
    frames: list[dict[str, Any]] = []
    header_seen = False
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {exc}") from exc
            if not header_seen:
                if not isinstance(record, dict) or record.get("format") != FORMAT:
                    raise ValueError(f"{path}: unsupported trace format")
                header_seen = True
                continue
            if not isinstance(record, dict) or not REQUIRED.issubset(record):
                raise ValueError(f"{path}:{line_number}: incomplete frame record")
            record["angles"] = _finite_vector(record["angles"], 3, "angles")
            record["origin"] = _finite_vector(record["origin"], 3, "origin")
            record["velocity"] = _finite_vector(record["velocity"], 3, "velocity")
            record["viewHeight"] = float(record["viewHeight"])
            if not math.isfinite(record["viewHeight"]):
                raise ValueError(f"{path}:{line_number}: viewHeight is non-finite")
            frames.append(record)
            if len(frames) > max_frames:
                raise ValueError(f"{path}: exceeds bounded frame count {max_frames}")
    if not header_seen or not frames:
        raise ValueError(f"{path}: no header or frame records")
    return frames


def _close_vector(left: list[float], right: list[float], tolerance: float) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(left, right))


def compare(
    pc: list[dict[str, Any]],
    vita: list[dict[str, Any]],
    angle_tolerance: float,
    position_tolerance: float,
    velocity_tolerance: float,
) -> tuple[bool, str]:
    if len(pc) != len(vita):
        return False, f"frame-count mismatch pc={len(pc)} vita={len(vita)}"
    exact_fields = ("frame", "time", "forward", "right", "buttons", "stance",
                    "weapon", "clip", "reserve", "health", "ads", "firing",
                    "reloading", "nextFire", "reloadStart", "reloadEnd")
    for index, (left, right) in enumerate(zip(pc, vita)):
        for field in exact_fields:
            if left[field] != right[field]:
                return False, f"frame {index} field {field}: pc={left[field]!r} vita={right[field]!r}"
        if not _close_vector(left["angles"], right["angles"], angle_tolerance):
            return False, f"frame {index} angles outside tolerance"
        if not _close_vector(left["origin"], right["origin"], position_tolerance):
            return False, f"frame {index} origin outside tolerance"
        if not _close_vector(left["velocity"], right["velocity"], velocity_tolerance):
            return False, f"frame {index} velocity outside tolerance"
        if abs(left["viewHeight"] - right["viewHeight"]) > position_tolerance:
            return False, f"frame {index} viewHeight outside tolerance"
    return True, f"traces match frames={len(pc)}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pc", required=True, type=Path, help="PC reference JSONL")
    parser.add_argument("--vita", required=True, type=Path, help="Vita JSONL")
    parser.add_argument("--angle-tolerance", type=float, default=1.0)
    parser.add_argument("--position-tolerance", type=float, default=0.01)
    parser.add_argument("--velocity-tolerance", type=float, default=0.05)
    parser.add_argument("--max-frames", type=int, default=100_000)
    args = parser.parse_args()
    if min(args.angle_tolerance, args.position_tolerance,
           args.velocity_tolerance) < 0 or args.max_frames <= 0:
        parser.error("tolerances must be non-negative and max-frames must be positive")
    try:
        pc = load_trace(args.pc, args.max_frames)
        vita = load_trace(args.vita, args.max_frames)
        matched, message = compare(pc, vita, args.angle_tolerance,
                                   args.position_tolerance, args.velocity_tolerance)
    except (OSError, ValueError) as exc:
        print(f"trace-compare: error: {exc}")
        return 2
    print(f"trace-compare: {'ok' if matched else 'mismatch'}: {message}")
    return 0 if matched else 1


if __name__ == "__main__":
    raise SystemExit(main())
