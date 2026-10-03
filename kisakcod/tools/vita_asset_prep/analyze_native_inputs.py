"""Check opt-in native texture/vertex controls without certifying game fidelity."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

WIDTH, HEIGHT = 960, 544
CONTROL_NAMES = ("linear", "dxt", "dxt-large", "static-zero", "static-offset",
                 "depth", "depth-reverse", "dynamic", "dynamic-crossing", "world-crossing")


def pixels(data: bytes) -> bytes:
    # The guest writes this exact bounded P6 contract, without optional comments.
    header = b"P6\n960 544\n255\n"
    if not data.startswith(header) or len(data) != len(header) + WIDTH * HEIGHT * 3:
        raise ValueError("Incomplete or incompatible 960x544 native P6 capture")
    return data[len(header):]


def expected_color(x: int, y: int) -> bytes:
    red, green, blue = x * 2, y * 4, 16
    return bytes(((red << 3) | (red >> 2), (green << 2) | (green >> 4),
                  (blue << 3) | (blue >> 2)))


def check_control(data: bytes, static: bool = False) -> int:
    image = pixels(data)
    # Sample block centers, away from the coverage edge and filtering seams.
    # These are independent clip-to-pixel expectations for the guest's known
    # vertices and affine matrix, including its native FBO readback orientation.
    left, bottom, width, height = (204, 81.6, 648, 326.4) if static else (48, 54.4, 864, 435.2)
    mismatches = 0
    for y in range(16):
        for x in range(16):
            px = int(left + (x + 0.5) * width / 16)
            py = int(bottom + (y + 0.5) * height / 16)
            start = (py * WIDTH + px) * 3
            mismatches += image[start:start + 3] != expected_color(x, y)
    return mismatches


def check_depth_control(data: bytes) -> int:
    image = pixels(data)
    mismatches = 0
    # A near green half-width/height square must cover a farther red plane,
    # regardless of submission order. Samples avoid all rasterization edges.
    for y in (54, 200, 340, 490):
        for x in (80, 320, 640, 880):
            start = (y * WIDTH + x) * 3
            expected = b"\x00\xff\x00" if 240 < x < 720 and 136 < y < 408 else b"\xff\x00\x00"
            mismatches += image[start:start + 3] != expected
    return mismatches


def check_dynamic_control(data: bytes, crossing: bool = False, world: bool = False) -> int:
    image = pixels(data)
    mismatches = 0
    # All sample points are strictly inside or outside the analytically
    # projected polygon. The crossing plane is z=1.25*x+3 in camera space;
    # clipping at world z=4 places its left edge at NDC x=0.2.
    samples = ((320, 200, True), (640, 340, True), (80, 270, False), (880, 270, False))
    if crossing:
        samples = ((80, 270, not world), (480, 270, not world),
                   (640, 270, True), (880, 270, False), (640, 20, False))
    for x, y, inside in samples:
        start = (y * WIDTH + x) * 3
        expected = b"\x00\xff\x00" if inside else b"\x00\x00\x00"
        mismatches += image[start:start+3] != expected
    return mismatches


def analyze(directory: Path) -> dict:
    controls = {}
    decoded = {}
    for name in CONTROL_NAMES:
        path = directory / f"native-contract-{name}.ppm"
        if not path.is_file():
            controls[name] = {"available": False, "passed": None}
            continue
        data = path.read_bytes()
        record = {"available": True, "sha256": hashlib.sha256(data).hexdigest()}
        try:
            if name.startswith("dynamic") or name == "world-crossing":
                record["sampleMismatches"] = check_dynamic_control(data, "crossing" in name, name == "world-crossing")
            else:
                record["sampleMismatches"] = (check_depth_control(data) if name.startswith("depth")
                                              else check_control(data, name.startswith("static")))
            record["passed"] = record["sampleMismatches"] == 0
            decoded[name] = pixels(data)
        except ValueError as error:
            record.update(passed=False, error=str(error))
        controls[name] = record
    pairs = {}
    for left, right in (("linear", "dxt"), ("linear", "dxt-large"),
                        ("static-zero", "static-offset"), ("depth", "depth-reverse")):
        pairs[f"{left}/{right}"] = (decoded[left] == decoded[right]
                                   if left in decoded and right in decoded else None)
    clear = directory / "native-clear.ppm"
    clear_mismatches = None
    if clear.is_file():
        try:
            raw = pixels(clear.read_bytes())
            clear_mismatches = sum(raw[start:start + 3] != b"\x0e\x13\x18"
                                   for start in range(0, len(raw), 3))
        except ValueError:
            clear_mismatches = WIDTH * HEIGHT
    identity = directory / "run.json"
    return {
        "schemaVersion": 1,
        "runIdentitySha256": hashlib.sha256(identity.read_bytes()).hexdigest() if identity.is_file() else None,
        "controls": controls, "pixelIdenticalPairs": pairs,
        "inputControlsPassed": all(c["passed"] is True for c in controls.values())
                               and all(p is True for p in pairs.values()),
        "clearPixelMismatches": clear_mismatches,
        "clearCoveragePassed": clear_mismatches == 0 if clear_mismatches is not None else None,
        "visuallyQualified": False, "performanceQualified": False, "hardwareQualified": False,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-directory", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.run_directory)
    (args.run_directory / "native-input-evidence.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
