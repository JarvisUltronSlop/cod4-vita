from __future__ import annotations

import importlib.util
import json
import tempfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("shipment_reference_package", ROOT / "shipment_reference_package.py")
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)
fastfile_spec = importlib.util.spec_from_file_location("fastfile_fixture", ROOT / "fastfile.py")
assert fastfile_spec and fastfile_spec.loader
fastfile = importlib.util.module_from_spec(fastfile_spec)
fastfile_spec.loader.exec_module(fastfile)


def test_reference_package_is_typed_but_not_playable() -> None:
    with tempfile.TemporaryDirectory() as root_name:
        root = Path(root_name)
        cod4 = root / "cod4"
        workspace = root / "workspace"
        (cod4 / "zone" / "english").mkdir(parents=True)
        (workspace / "assets").mkdir(parents=True)
        load = cod4 / "zone" / "english" / "mp_shipment_load.ff"
        map_file = cod4 / "zone" / "english" / "mp_shipment.ff"
        load_payload = b"$victorybackdrop\0loadscreen_mp_shipment\0"
        map_payload = b"maps/mp/mp_shipment.d3dbsp\0mc/mtl_cargocontainer\0sound/weapons/m4_fire.wav\0"
        for path, payload in ((load, load_payload), (map_file, map_payload)):
            path.write_bytes(fastfile.HEADER.pack(fastfile.MAGIC, 5) + zlib.compress(payload))

        output = workspace / "assets" / "packages" / "shipment-reference.kvap"
        report = module.build_reference_package(load, map_file, workspace, output)
        assert report["status"] == "reference-only-pending-typed-db-export"
        assert report["playable"] is False
        assert {chunk["type"] for chunk in report["chunks"]} >= {1, 2, 4, 5, 13}
        manifest = workspace / "assets" / "converted" / "shipment-reference" / "manifest.json"
        text = manifest.read_text(encoding="utf-8")
        assert "D:" not in text
        assert json.loads(text)["playable"] is False


if __name__ == "__main__":
    test_reference_package_is_typed_but_not_playable()
    print("shipment reference package tests: ok")
