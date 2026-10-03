#!/usr/bin/env python3
"""Summarize one guarded Vita3K Shipment run without changing the guest.

The Vita bootstrap deliberately keeps its runtime diagnostics in two bounded
JSONL streams: stdout carries the boot/package/renderer gates and the guest
trace carries per-frame input and player state.  This report joins those
streams, tolerates a stopped emulator leaving a partial final line, and keeps
unsupported paths visible instead of treating them as successes.
"""

from __future__ import annotations

import argparse
from collections import Counter
import json
import math
import re
import sys
from pathlib import Path
from typing import Any


TRACE_FORMAT = "CODVIT-SHIPMENT-TRACE-2"
REPORT_FORMAT = "CODVIT-RUNTIME-REPORT-1"


def _flag(value: str) -> bool:
    return value == "1"


def parse_stdout(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8", errors="replace")

    profile_match = re.search(
        r"profile name=([^\s]+) active=(\d+) config=(\d+)", text
    )
    shipment_match = re.search(r"shipment playable=(\d+)", text)
    dm_match = re.search(
        r"local Shipment DM ready=(\d+) clients=(\d+)", text
    )
    audio_match = re.search(r"audio ready=(\d+) port=(-?\d+)", text)
    shader_module_path = re.search(
        r'Loading module "([^"]*libshacccg\.suprx)"', text
    )
    shader_module_loaded = bool(
        shader_module_path
        and re.search(r"Module SceShaccCg .* loaded", text)
    )
    gxm_stubbed = "Stubbed sceGxm" in text

    if re.search(r"renderer fallback state=1", text):
        renderer_state = "software_fallback"
    elif re.search(r"renderer accelerated state=1", text):
        renderer_state = "accelerated"
    elif "renderer unavailable" in text:
        renderer_state = "unavailable"
    else:
        renderer_state = "unknown"

    return {
        "profile": {
            "name": profile_match.group(1) if profile_match else None,
            "active": _flag(profile_match.group(2)) if profile_match else False,
            "config": _flag(profile_match.group(3)) if profile_match else False,
            "userReady": bool(
                profile_match
                and profile_match.group(1) == "User"
                and profile_match.group(2) == "1"
                and profile_match.group(3) == "1"
            ),
        },
        "shipmentPlayable": bool(
            shipment_match and _flag(shipment_match.group(1))
        ),
        "localDm": {
            "ready": bool(dm_match and _flag(dm_match.group(1))),
            "clients": int(dm_match.group(2)) if dm_match else 0,
        },
        "renderer": {
            "state": renderer_state,
            "softwareFramebuffer": "software framebuffer fallback" in text,
            "ready": renderer_state in {"software_fallback", "accelerated"},
            "shaderCompiler": {
                "loaded": shader_module_loaded,
                "path": shader_module_path.group(1) if shader_module_path else None,
            },
            "gxmStubbed": gxm_stubbed,
        },
        "audio": {
            "reported": audio_match is not None,
            "ready": bool(audio_match and _flag(audio_match.group(1))),
            "port": int(audio_match.group(2)) if audio_match else None,
        },
        "stdoutBytes": len(text.encode("utf-8")),
    }


def _number(value: Any, default: float = 0.0) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return default
    return number if math.isfinite(number) else default


def parse_jsonl(path: Path | None, *, trace: bool) -> dict[str, Any]:
    empty = {
        "present": False,
        "format": None,
        "header": False,
        "invalidLines": 0,
        "lineCount": 0,
        "markers": [],
        "frames": 0,
        "firstFrame": None,
        "lastFrame": None,
        "movementObserved": False,
        "firingObserved": False,
        "reloadObserved": False,
        "adsObserved": False,
        "adsFractionMax": 0.0,
        "weapons": [],
        "usable": False,
    }
    if path is None:
        return empty

    result = dict(empty)
    result["present"] = path.is_file()
    if not result["present"]:
        return result

    frames: list[dict[str, Any]] = []
    origins: list[list[float]] = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        result["present"] = False
        return result
    result["lineCount"] = len(lines)
    for line in lines:
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            result["invalidLines"] += 1
            continue
        if not isinstance(record, dict):
            result["invalidLines"] += 1
            continue
        if "format" in record and not result["header"]:
            result["format"] = record.get("format")
            result["header"] = record.get("format") == TRACE_FORMAT
            continue
        if "marker" in record:
            marker = record.get("marker")
            if isinstance(marker, str):
                result["markers"].append(marker)
            else:
                result["invalidLines"] += 1
            continue
        if not trace and "stage" in record:
            stage = record.get("stage")
            if isinstance(stage, str):
                result["markers"].append(stage)
            else:
                result["invalidLines"] += 1
            continue
        if not trace or "frame" not in record:
            result["invalidLines"] += 1
            continue
        if not all(field in record for field in ("origin", "velocity", "weapon")):
            result["invalidLines"] += 1
            continue
        origin = record.get("origin")
        velocity = record.get("velocity")
        if (
            not isinstance(origin, list)
            or len(origin) != 3
            or not isinstance(velocity, list)
            or len(velocity) != 3
        ):
            result["invalidLines"] += 1
            continue
        frame = record
        frames.append(frame)
        origins.append([_number(value) for value in origin])

    result["frames"] = len(frames)
    if frames:
        result["firstFrame"] = frames[0].get("frame")
        result["lastFrame"] = frames[-1].get("frame")
    result["movementObserved"] = any(
        any(abs(_number(value)) > 1e-4 for value in frame.get("velocity", []))
        or abs(_number(frame.get("forward"))) > 0
        or abs(_number(frame.get("right"))) > 0
        for frame in frames
    )
    result["firingObserved"] = any(_number(frame.get("firing")) != 0 for frame in frames)
    result["reloadObserved"] = any(
        _number(frame.get("reloading")) != 0
        or _number(frame.get("reloadStart")) != 0
        or _number(frame.get("reloadEnd")) != 0
        for frame in frames
    )
    ads_fractions = []
    for frame in frames:
        raw_fraction = frame.get("adsFrac")
        if raw_fraction is None:
            raw_fraction = 1.0 if _number(frame.get("ads")) != 0 else 0.0
        ads_fractions.append(max(0.0, min(1.0, _number(raw_fraction))))
    result["adsFractionMax"] = max(ads_fractions, default=0.0)
    result["adsObserved"] = any(value > 1e-3 for value in ads_fractions)
    result["weapons"] = sorted(
        {int(_number(frame.get("weapon"))) for frame in frames if 1 <= _number(frame.get("weapon")) <= 3}
    )
    result["usable"] = bool(result["header"] and frames)
    return result


def build_report(stdout: Path, script_stages: Path | None,
                 shipment_trace: Path | None) -> dict[str, Any]:
    boot = parse_stdout(stdout)
    trace = parse_jsonl(shipment_trace, trace=True)
    stages = parse_jsonl(script_stages, trace=False)
    stage_counts = dict(sorted(Counter(stages["markers"]).items()))
    gameplay = {
        "traceUsable": trace["usable"],
        "frames": trace["frames"],
        "firstFrame": trace["firstFrame"],
        "lastFrame": trace["lastFrame"],
        "movementObserved": trace["movementObserved"],
        "firingObserved": trace["firingObserved"],
        "reloadObserved": trace["reloadObserved"],
        "adsObserved": trace["adsObserved"],
        "adsFractionMax": trace["adsFractionMax"],
        "weapons": trace["weapons"],
    }
    gates = {
        "profileUser": boot["profile"]["userReady"],
        "shipmentPlayable": boot["shipmentPlayable"],
        "localDm": boot["localDm"]["ready"] and boot["localDm"]["clients"] == 1,
        "renderer": boot["renderer"]["ready"],
        "gameplay": trace["usable"],
        "ads": trace["adsObserved"] and trace["adsFractionMax"] >= 0.9,
    }
    gates["coreVerticalSlice"] = all(gates.values())
    return {
        "format": REPORT_FORMAT,
        "boot": boot,
        "gameplay": gameplay,
        "trace": {
            "path": str(shipment_trace) if shipment_trace else None,
            "present": trace["present"],
            "format": trace["format"],
            "invalidLines": trace["invalidLines"],
            "lineCount": trace["lineCount"],
            "markers": trace["markers"],
            "usable": trace["usable"],
        },
        "scriptStages": {
            "path": str(script_stages) if script_stages else None,
            "present": stages["present"],
            "format": stages["format"],
            "invalidLines": stages["invalidLines"],
            "lineCount": stages["lineCount"],
            # Script tracing is intentionally noisy.  Keep the report small
            # and deterministic while retaining the complete stage vocabulary
            # and counts for qualification/debugging.
            "stages": sorted(stage_counts),
            "stageCounts": stage_counts,
            "nativeEntityQualificationDeferred": "entity_runtime_deferred" in stages["markers"],
        },
        "gates": gates,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdout", required=True, type=Path, help="guarded Vita3K stdout capture")
    parser.add_argument("--script-stages", type=Path, help="guest script-stages.jsonl")
    parser.add_argument("--shipment-trace", type=Path, help="guest shipment-trace.jsonl")
    parser.add_argument("--output", type=Path, help="write the report JSON to this path")
    args = parser.parse_args()
    try:
        report = build_report(args.stdout, args.script_stages, args.shipment_trace)
    except OSError as exc:
        print(f"vita-runtime-report: error: {exc}", file=sys.stderr)
        return 2
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        try:
            args.output.write_text(rendered, encoding="utf-8")
        except OSError as exc:
            print(f"vita-runtime-report: error: {exc}", file=sys.stderr)
            return 2
    else:
        print(rendered, end="")
    print(
        "vita-runtime-report: "
        + ("core gates passed" if report["gates"]["coreVerticalSlice"] else "core gates incomplete"),
        file=sys.stderr,
    )
    return 0 if report["gates"]["coreVerticalSlice"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
