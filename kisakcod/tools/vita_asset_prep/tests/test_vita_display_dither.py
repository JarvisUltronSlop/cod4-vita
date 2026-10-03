"""Verify scalar/native display quantisation uses one deterministic table."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BACKEND = ROOT / "src" / "gfx_vita" / "gfx_vita_backend.cpp"


def main() -> int:
    backend = BACKEND.read_text(encoding="utf-8")
    table_match = re.search(
        r"kDisplayDither8x8\[64\]\s*=\s*\{(?P<body>.*?)\};",
        backend,
        re.DOTALL,
    )
    assert table_match, "shared 8x8 dither table is missing"
    values = [
        int(value)
        for value in re.findall(r"\b(\d+)u\b", table_match.group("body"))
    ]
    assert len(values) == 64
    assert sorted(values) == list(range(64))
    assert "float DisplayDither8x8(vec2 position)" in backend
    assert "const float dither = DisplayDither8x8(x, y);" in backend
    assert "float dither = (DisplayDither8x8(ditherPosition) - 31.5)" in backend
    assert "(64.0 * 255.0)" in backend
    assert "Bayer4" not in backend
    assert "ordered4x4" not in backend
    print("vita display dither tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
