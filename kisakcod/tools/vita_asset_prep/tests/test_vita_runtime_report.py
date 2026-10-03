#!/usr/bin/env python3
"""Regression checks for the guarded Vita runtime qualification report."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "vita_runtime_report.py"


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(REPORT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        stdout = root / "stdout.txt"
        trace = root / "shipment-trace.jsonl"
        stages = root / "script-stages.jsonl"
        output = root / "report.json"
        stdout.write_text(
            "\n".join(
                [
                    "[kisak-vita] profile name=User active=1 config=1",
                    "[kisak-vita] shipment playable=1",
                    "[kisak-vita] local Shipment DM ready=1 clients=1",
                    "[kisak-vita] renderer fallback state=1",
                    "[kisak-vita] software framebuffer fallback result=0 size=960x544",
                    "[kisak-vita] audio ready=0 port=-1",
                    '[load_module]: Loading module "ur0:/data/libshacccg.suprx"',
                    "Module SceShaccCg (at ur0:/data/libshacccg.suprx) loaded",
                    "Stubbed sceGxmVshInitialize import called",
                ]
            ),
            encoding="utf-8",
        )
        frame = {
            "frame": 3,
            "forward": 96,
            "right": 0,
            "origin": [1, 2, 3],
            "velocity": [1, 0, 0],
            "weapon": 2,
            "ads": 1,
            "adsFrac": 1.0,
            "firing": 1,
            "reloading": 1,
            "reloadStart": 100,
            "reloadEnd": 500,
        }
        trace.write_text(
            json.dumps({"format": "CODVIT-SHIPMENT-TRACE-2"})
            + "\n"
            + json.dumps({"marker": "frame-loop-ready"})
            + "\nnot-json\n"
            + json.dumps(frame)
            + "\n",
            encoding="utf-8",
        )
        stages.write_text(
            json.dumps({"stage": "vita_dm_state", "subject": "ready"})
            + "\n"
            + json.dumps({"stage": "entity_runtime_deferred"})
            + "\n",
            encoding="utf-8",
        )
        result = run(
            "--stdout",
            str(stdout),
            "--shipment-trace",
            str(trace),
            "--script-stages",
            str(stages),
            "--output",
            str(output),
        )
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads(output.read_text(encoding="utf-8"))
        assert report["gates"]["coreVerticalSlice"] is True
        assert report["gameplay"]["movementObserved"] is True
        assert report["gameplay"]["firingObserved"] is True
        assert report["gameplay"]["reloadObserved"] is True
        assert report["gameplay"]["adsObserved"] is True
        assert report["gameplay"]["adsFractionMax"] == 1.0
        assert report["gameplay"]["weapons"] == [2]
        assert report["gates"]["ads"] is True
        assert report["boot"]["renderer"]["shaderCompiler"]["loaded"] is True
        assert report["boot"]["renderer"]["shaderCompiler"]["path"] == "ur0:/data/libshacccg.suprx"
        assert report["boot"]["renderer"]["gxmStubbed"] is True
        assert report["trace"]["invalidLines"] == 1
        assert report["scriptStages"]["nativeEntityQualificationDeferred"] is True

        stdout.write_text(
            "[kisak-vita] profile name=User active=1 config=1\n"
            "[kisak-vita] shipment playable=1\n"
            "[kisak-vita] local Shipment DM ready=1 clients=1\n"
            "[kisak-vita] renderer unavailable; ending bootstrap cleanly\n",
            encoding="utf-8",
        )
        result = run("--stdout", str(stdout), "--shipment-trace", str(trace))
        assert result.returncode == 1, result.stdout + result.stderr
        report = json.loads(result.stdout)
        assert report["boot"]["renderer"]["state"] == "unavailable"
        assert report["gates"]["coreVerticalSlice"] is False

    print("vita runtime report tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
