#!/usr/bin/env python3
"""Regression checks for streamed XModel/viewmodel semantic materials."""

from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "tools" / "vita_asset_prep"
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"
MAP_ASSETS = ROOT / "src" / "vita" / "vita_map_assets.h"
GFX_HEADER = ROOT / "src" / "gfx" / "gfx_backend.h"
EXPORTER = TOOLS / "map_bundle_export.py"
TYPED_EXPORTER = TOOLS / "shipment_typed_export.py"
CMAKE = ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt"
RUNNER = ROOT / "tools" / "vita" / "run_guarded_vita3k.ps1"
BUILD_HELPER = ROOT.parent / "build-vita.ps1"


def load_typed_exporter():
    spec = importlib.util.spec_from_file_location(
        "shipment_typed_export_model_semantics", TYPED_EXPORTER
    )
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_vitp_viewmodel_names_use_the_wire_offset() -> None:
    module = load_typed_exporter()
    with tempfile.TemporaryDirectory() as root_name:
        path = Path(root_name) / "vita_weapons.bin"
        records = []
        for name in ("viewmodel_m4_mp", "viewmodel_usp_mp"):
            record = bytearray(2360)
            record[160:160 + len(name)] = name.encode("utf-8")
            records.append(record)
        path.write_bytes(
            struct.pack("<4I", 0x50544956, 4, len(records), 2360)
            + b"".join(records)
        )
        assert module._weapon_view_model_names(path) == {
            "viewmodel_m4_mp", "viewmodel_usp_mp"
        }


def main() -> int:
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
    map_assets = MAP_ASSETS.read_text(encoding="utf-8")
    gfx_header = GFX_HEADER.read_text(encoding="utf-8")
    exporter = EXPORTER.read_text(encoding="utf-8")
    typed_exporter = TYPED_EXPORTER.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    runner = RUNNER.read_text(encoding="utf-8")
    build_helper = BUILD_HELPER.read_text(encoding="utf-8")

    assert "VitaMapModelNormalImagesAssetId" in map_assets
    assert "VitaMapModelSpecularImagesAssetId" in map_assets
    assert "VitaMapModelColorImagesAssetId" in map_assets
    assert "0x4D49040000000000ull" in map_assets
    assert "0x4D49050000000000ull" in map_assets
    assert "0x4D49060000000000ull" in map_assets

    assert '"modelNormalImages": 0x4D49040000000000' in exporter
    assert '"modelSpecularImages": 0x4D49050000000000' in exporter
    assert "MODEL_SEMANTIC_IMAGE_SCOPE = \"model\"" in exporter
    assert "def build_model_semantic_package" in exporter
    assert "--model-semantic-output" in exporter
    assert "VitaMapModelNormalImagesAssetId" not in exporter

    assert "def _weapon_view_model_names" in typed_exporter
    assert "extra_model_names" in typed_exporter
    assert "entity_models.update(_weapon_view_model_names" in typed_exporter
    assert 'entity_models.add("viewmodel_base_viewhands")' in typed_exporter
    assert 'scope == "model"' in typed_exporter
    assert "view_model_offset = 64 + 17 * 4 + 7 * 4" in typed_exporter

    assert "GFX_MATERIAL_PASS_BAKED_LIGHTING" in gfx_header
    assert "LoadMapModelImageSegmentFromPackage" in bootstrap
    assert "LoadModelColorImagesFromPackage" in bootstrap
    assert "modelColorImages.Clear()" in bootstrap
    assert "StageNativeModelSemanticCompanion" in bootstrap
    assert "DeriveModelSemanticPackagePath" in bootstrap
    assert "nativeWorldNormalSegments" in bootstrap
    assert "nativeModelSpecularSegments" in bootstrap
    assert "native semantic companions staged" in bootstrap
    assert "modelNormalTextureHandles" in bootstrap
    assert "modelSpecularTextureHandles" in bootstrap
    assert "images->ReleaseAllPayloads()" in bootstrap
    assert "GFX_MATERIAL_PASS_BAKED_LIGHTING" in bootstrap
    assert "kScalarModelNormalPayloadBudget" in bootstrap
    assert "kScalarModelSpecularPayloadBudget" in bootstrap
    assert "normalBudgetBytes" in bootstrap
    assert "specularBudgetBytes" in bootstrap
    assert "viewmodelsOnly" in bootstrap
    # Scalar XModels must install their own descriptor even though the
    # semantic normal/specular response is pre-evaluated into vertex colour;
    # otherwise a prop can inherit the last BSP material pass.
    assert "nativeSourceLitModel || (softwareFallback && gfx->SetMaterialPass)" in bootstrap
    assert "pre-evaluated bridge is not applied a second time" in bootstrap

    database_registry = (ROOT / "src" / "database" / "db_registry.cpp").read_text(
        encoding="utf-8")
    map_bundle = EXPORTER.read_text(encoding="utf-8")
    assert "DB_VitaCollectFxModels" in database_registry
    assert "reserved FX slots" in database_registry
    assert "def _effect_model_names" in map_bundle
    assert "effect_model_names" in map_bundle

    assert "KISAK_VITA_MODEL_SEMANTIC_PACKAGE" in cmake
    assert "shipment-db-model-semantics.kvap" in cmake
    assert 'FILE "${KISAK_VITA_MODEL_SEMANTIC_PACKAGE}"' in cmake
    assert "ModelSemanticPackagePath" in runner
    assert "shipment-db-model-semantics.kvap" in runner
    assert "INSTALLED_APP0_MODEL_SEMANTIC_PACKAGE_HARDLINK" in runner
    assert "mp-map-model-semantics.kvap" in build_helper
    assert "KISAK_VITA_MODEL_SEMANTIC_PACKAGE" in build_helper

    test_vitp_viewmodel_names_use_the_wire_offset()
    print("vita model semantic tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
