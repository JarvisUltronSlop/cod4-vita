from __future__ import annotations

import struct
import tempfile
from pathlib import Path


HEADER = struct.Struct("<8I")


def validate(path: Path) -> tuple[int, int, int, int]:
    data = path.read_bytes()
    magic, version, planes, brushes, sides, vertices, triangles, submodels = HEADER.unpack_from(data)
    assert magic == 0x43544956
    assert version in (2, 3)
    legacy_bytes = (32 + planes * 18 + brushes * 36 + sides * 24
                    + vertices * 12 + triangles * 3 * 2 + submodels * 28)
    expected = legacy_bytes
    if version == 3:
        tail = legacy_bytes + sides * 4
        model_count, membership_count = struct.unpack_from("<2I", data, tail)
        assert model_count == submodels
        offsets = struct.unpack_from(f"<{submodels + 1}I", data, tail + 8)
        assert offsets[0] == 0
        assert offsets[-1] == membership_count
        expected = tail + 8 + (submodels + 1) * 4 + membership_count * 2
    assert expected == len(data)
    cursor = 32 + planes * 18
    brush_records = []
    for _ in range(brushes):
        side_offset = struct.unpack_from("<I", data, cursor + 28)[0]
        side_count = struct.unpack_from("<I", data, cursor + 32)[0]
        assert side_offset + side_count <= sides
        brush_records.append((side_offset, side_count))
        cursor += 36
    cursor += sides * 24
    assert cursor + vertices * 12 + triangles * 3 * 2 + submodels * 28 == legacy_bytes
    return planes, brushes, sides, vertices


def test_collision_sidecar_layout() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_collision.bin"
        path.write_bytes(
            HEADER.pack(0x43544956, 2, 1, 1, 1, 1, 1, 1)
            + bytes(18) + bytes(36) + bytes(24) + bytes(12) + bytes(6) + bytes(28)
        )
        assert validate(path) == (1, 1, 1, 1)


def test_source_complete_collision_sidecar_layout() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_collision.bin"
        legacy = (
            HEADER.pack(0x43544956, 3, 1, 1, 1, 1, 1, 1)
            + bytes(18) + bytes(36) + bytes(24) + bytes(12)
            + bytes(6) + bytes(28)
        )
        # One side surface flag, one cmodel, one sorted world brush index.
        path.write_bytes(
            legacy + struct.pack("<i2I2IH", 7, 1, 1, 0, 1, 0)
        )
        assert validate(path) == (1, 1, 1, 1)


def test_map_entity_sidecar_layout() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_map_ents.bin"
        entity_text = b"{\\\"classname\\\":\\\"mp_dm_spawn\\\"}"
        path.write_bytes(struct.pack("<3I", 0x45544956, 1, len(entity_text)) + entity_text)
        magic, version, length = struct.unpack_from("<3I", path.read_bytes())
        assert (magic, version, length) == (0x45544956, 1, len(entity_text))


if __name__ == "__main__":
    test_collision_sidecar_layout()
    test_source_complete_collision_sidecar_layout()
    test_map_entity_sidecar_layout()
    print("shipment collision sidecar tests: ok")
