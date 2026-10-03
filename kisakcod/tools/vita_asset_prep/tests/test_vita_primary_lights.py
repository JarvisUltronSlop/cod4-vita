#!/usr/bin/env python3
"""Regression checks for the pointer-free VITP primary-light sidecar."""

import struct
import tempfile
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "tools" / "vita_asset_prep"
sys.path.insert(0, str(TOOLS))
import shipment_typed_export as typed  # noqa: E402


def _record(light_type: int = 1) -> bytes:
    return struct.pack(
        "<4B15f",
        light_type, 1, 0, 0,
        1.0, 0.8, 0.6,
        0.0, 0.0, -1.0,
        128.0, 64.0, 32.0,
        256.0,
        0.8, 0.9, 1.0,
        0.0, 0.0,
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        sidecar = root / "vita_primary_lights.bin"
        sidecar.write_bytes(struct.pack("<5I", 0x50544956, 1, 1, 64, 0)
                            + _record())
        info = typed._validate_primary_light_sidecar(sidecar)
        assert info is not None
        assert info["records"] == 1
        assert info["sunIndex"] == 0

        bad = root / "bad.bin"
        bad.write_bytes(struct.pack("<5I", 0x50544956, 1, 1, 64, 0)
                        + _record(7))
        try:
            typed._validate_primary_light_sidecar(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid VITP light type was accepted")

        world = root / "vita_world.bin"
        # VITW v2 header plus bounds and the 76-byte authored lighting record.
        lighting = struct.pack(
            "<15fI3f",
            0.0, 0.0, -1.0,
            0.35, 0.42, 0.50,
            0.1, 0.1, 0.1,
            0.8, 0.8, 0.8,
            0.0, 0.0, 1.0,
            0,
            0.0, 0.0, 0.0,
        )
        world.write_bytes(struct.pack("<5I", 0x57544956, 2, 0, 0, 0)
                          + bytes(24) + lighting)
        synthesized = typed._synthesize_primary_light_sidecar(
            world, root / "synthesized.bin")
        assert synthesized is not None
        assert synthesized["compatibilitySource"] == "VITW v2 authored sun"

    print("vita primary-light tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
