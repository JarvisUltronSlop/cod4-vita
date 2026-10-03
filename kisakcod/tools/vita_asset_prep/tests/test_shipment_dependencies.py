from __future__ import annotations

import importlib.util
import hashlib
import tempfile
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "shipment_dependencies.py"
SPEC = importlib.util.spec_from_file_location("shipment_dependencies", MODULE)
assert SPEC and SPEC.loader
deps = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(deps)


def test_inventory_hashes_exact_sources() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        (root / "zone" / "english").mkdir(parents=True)
        load = root / "zone" / "english" / "mp_shipment_load.ff"
        world = root / "zone" / "english" / "mp_shipment.ff"
        load.write_bytes(b"load")
        world.write_bytes(b"world")
        result = deps.build_manifest(root)
        assert result["readOnly"] is True
        assert result["status"] == "source-verified-conversion-pending"
        entries = result["entries"]
        assert len(entries) == 2
        assert entries[0]["bytes"] == 4
        assert entries[0]["sha256"] == hashlib.sha256(b"load").hexdigest()
        assert entries[1]["expectedTypes"] == [
            "world", "collision", "material", "image", "model", "weapon", "sound", "hud"
        ]


if __name__ == "__main__":
    test_inventory_hashes_exact_sources()
    print("shipment dependency tests: ok")
