import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPARE = ROOT / "trace_compare.py"
HEADER = {"format": "CODVIT-SHIPMENT-TRACE-2"}


def record(origin=(1.0, 2.0, 3.0)):
    return {
        "frame": 0,
        "time": 16,
        "forward": 127,
        "right": 0,
        "buttons": 1,
        "angles": [0, 90, 0],
        "origin": list(origin),
        "velocity": [0.0, 1.0, 0.0],
        "stance": "stand",
        "weapon": 1,
        "clip": 29,
        "reserve": 90,
        "health": 100,
        "ads": 0,
        "firing": 0,
        "reloading": 0,
        "viewHeight": 60.0,
        "nextFire": 100,
        "reloadStart": 0,
        "reloadEnd": 0,
    }


def write_trace(path, frame):
    path.write_text(json.dumps(HEADER) + "\n" + json.dumps(frame) + "\n",
                    encoding="utf-8")


def run(pc, vita):
    return subprocess.run([sys.executable, str(COMPARE), "--pc", str(pc),
                           "--vita", str(vita)], capture_output=True,
                          text=True, check=False)


def main():
    with tempfile.TemporaryDirectory() as directory:
        pc = Path(directory) / "pc.jsonl"
        vita = Path(directory) / "vita.jsonl"
        write_trace(pc, record())
        write_trace(vita, record((1.005, 2.0, 3.0)))
        result = run(pc, vita)
        assert result.returncode == 0, result.stdout + result.stderr
        write_trace(vita, record((1.1, 2.0, 3.0)))
        result = run(pc, vita)
        assert result.returncode == 1, result.stdout + result.stderr
        write_trace(vita, {**record(), "reloadEnd": 1200})
        result = run(pc, vita)
        assert result.returncode == 1, result.stdout + result.stderr
    print("vita trace comparison tests: ok")


if __name__ == "__main__":
    main()
