#!/usr/bin/env python3
"""Extract actual original client commands/states from diagnostic Vita runs.

These are observations of a controller stimulus, not Windows/Vita parity or
performance qualification. Phase labels describe the requested input only.
"""
from __future__ import annotations

import json
import math
import re
from pathlib import Path


def extract(log: bytes) -> tuple[dict, list[dict]]:
    phases: dict[int, dict] = {}
    records: list[dict] = []
    loaded = completed = None
    malformed = 0
    for line in log.decode("utf-8", errors="replace").splitlines():
        if match := re.search(r"source input smoke loaded steps=(\d+) minimumMs=(\d+)", line):
            loaded = {"steps": int(match[1]), "minimumMs": int(match[2])}
        if match := re.search(r"source input smoke completed steps=(\d+) elapsedMs=(\d+)", line):
            completed = {"steps": int(match[1]), "elapsedMs": int(match[2])}
        if match := re.search(r"source input phase=(\d+) name=(\w+) time=(\d+)", line):
            phases[int(match[1])] = {"name": match[2], "startTime": int(match[3]),
                "commands": 0, "states": {}}
        if not (match := re.search(r"source input (cmd|state) (.+)$", line)):
            continue
        row: dict = {"kind": match[1]}
        try:
            for key, value in re.findall(r"(\w+)=([^ ]+)", match[2]):
                if key == "owner":
                    if value not in ("client", "server"):
                        raise ValueError("unknown player state owner")
                    row[key] = value
                elif key in ("buttons", "flags"):
                    row[key] = int(value, 16)
                elif key in ("ads", "pos", "vel", "angles", "move"):
                    numbers = [float(number) for number in value.split(",")]
                    if not all(map(math.isfinite, numbers)):
                        raise ValueError("nonfinite gameplay observation")
                    expected = 3 if key in ("pos", "vel", "angles") else 2 if key == "move" else 1
                    if len(numbers) != expected:
                        raise ValueError("invalid observed vector")
                    if row["kind"] == "cmd":
                        if any(number != int(number) for number in numbers):
                            raise ValueError("nonintegral usercmd")
                        numbers = list(map(int, numbers))
                    row[key] = numbers[0] if key == "ads" else numbers
                else:
                    row[key] = int(value)
            phase = phases[row["phase"]]
            if row["kind"] == "cmd":
                for key in ("number", "time", "buttons", "weapon", "offhand", "move", "angles", "rejected"):
                    if key not in row:
                        raise ValueError("incomplete command")
                phase["commands"] += 1
            else:
                for key in ("owner", "time", "pm", "flags", "weapon", "weaponState", "health", "clip", "ammo", "ads", "pos"):
                    if key not in row:
                        raise ValueError("incomplete player state")
                state = phase["states"].setdefault(row["owner"], {"samples": 0,
                    "firstTime": row["time"], "firstPosition": row["pos"],
                    **{key: set() for key in ("pm", "flags", "weapon", "weaponState", "health", "clip", "ammo")},
                    "adsMinimum": row["ads"], "adsMaximum": row["ads"]})
                state["samples"] += 1
                state["lastTime"], state["lastPosition"] = row["time"], row["pos"]
                state["adsMinimum"] = min(state["adsMinimum"], row["ads"])
                state["adsMaximum"] = max(state["adsMaximum"], row["ads"])
                for key in ("pm", "flags", "weapon", "weaponState", "health", "clip", "ammo"):
                    state[key].add(row[key])
            records.append(row)
        except (ValueError, KeyError, OverflowError):
            malformed += 1
    for phase in phases.values():
        for state in phase["states"].values():
            for key, value in state.items():
                if isinstance(value, set):
                    state[key] = sorted(value)
    return {"schemaVersion": 1, "loaded": loaded, "completed": completed,
        "malformedRecords": malformed, "records": len(records),
        "phases": {str(key): value for key, value in phases.items()},
        "windowsVitaParityQualified": False, "hardwareQualified": False}, records


def write_report(root: Path, log: bytes, identity_hash: str) -> None:
    report, records = extract(log)
    if not report["loaded"]:
        return
    report["runIdentitySha256"] = identity_hash
    (root / "source-input-evidence.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    with (root / "source-input-trace.jsonl").open("w", encoding="utf-8") as stream:
        for record in records:
            stream.write(json.dumps(record, separators=(",", ":"), allow_nan=False) + "\n")
