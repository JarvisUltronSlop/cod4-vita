#!/usr/bin/env python3
"""Regression tests for exact shared effect-image closure repair."""

from __future__ import annotations

import importlib.util
from pathlib import Path


MODULE = (Path(__file__).resolve().parents[1]
          / "repair_map_image_closures.py")
SPEC = importlib.util.spec_from_file_location("repair_map_image_closures", MODULE)
assert SPEC and SPEC.loader
repair = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(repair)


def image(payload: bytes) -> repair.ImageRecord:
    return (len(payload), 4, 4, 1, 1, 21, 0, payload)


def test_shared_effect_images_are_exactly_reused() -> None:
    existing = repair._write_image_records((
        ("farm_only", image(b"farm")),
        ("shared", image(b"farm-shared")),
    ))
    merged, count, names = repair._merge_exact_effect_images(
        existing,
        {
            "shared": image(b"other-shared"),
            "global_smoke": image(b"source-smoke"),
        },
    )
    records = repair._image_records(merged)
    assert count == 1
    assert names == ["global_smoke"]
    assert records["farm_only"][7] == b"farm"
    assert records["shared"][7] == b"farm-shared"
    assert records["global_smoke"][7] == b"source-smoke"


if __name__ == "__main__":
    test_shared_effect_images_are_exactly_reused()
    print("repair effect image tests: ok")
