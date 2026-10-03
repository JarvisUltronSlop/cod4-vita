"""Validate catalog routing and the retained rendering policy contracts."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MAP_HEADER = ROOT / "src" / "vita" / "vita_map_assets.h"
PACKER = ROOT / "tools" / "vita_asset_prep" / "vertical_slice_packs.py"
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"


MAP_COUNT = 20
CERTIFIED = set(range(20)) - {13, 14, 17, 18}


def main() -> int:
    header = MAP_HEADER.read_text(encoding="utf-8")
    packer = PACKER.read_text(encoding="utf-8")
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")

    assert "inline constexpr size_t kVitaMpMapCount" in header
    assert '#include "vita_map_catalog.inc"' in header
    assert "VitaMapIsEnabled" in header and "VitaFindMapIndex" in header
    assert 'MAPS = {0: "shipment", 2: "crossfire"}' in packer
    assert "CPU_BUDGET = 114 * 1024 * 1024" in packer
    assert "GPU_BUDGET = 64 * 1024 * 1024" in packer
    assert "WORKING_BUDGET = 178 * 1024 * 1024" in packer
    assert "common.kvap" in packer
    assert 'f"mp_{map_name}.kvap"' in packer
    for family in (
        "VitaMapWorldAssetId", "VitaMapCollisionAssetId",
        "VitaMapLightGridAssetId", "VitaMapEntitiesAssetId",
        "VitaMapModelsAssetId", "VitaMapMaterialsAssetId",
        "VitaMapImagesAssetId", "VitaMapHudAssetId",
        "VitaMapEnvironmentAssetId", "VitaMapPrimaryLightsAssetId",
        "VitaMapEffectsAssetId", "VitaMapNormalImagesAssetId",
        "VitaMapSpecularImagesAssetId", "VitaMapColorImagesAssetId",
    ):
        assert family in bootstrap
    assert "map closure audit failed" in bootstrap
    assert "availableMapMask" in bootstrap
    assert "VitaContentManifest_ValidateDependency" in bootstrap
    assert '"ux0:data/CODVIT001/paks/common.kvap"' in bootstrap
    assert "kVitaGlTransientPoolBytes = 2u * 1024u * 1024u" in bootstrap
    assert "kNativeViewModelBufferCount = 5u" in bootstrap
    assert "kVitaGpuPoolBytes = 64u * 1024u * 1024u" in bootstrap
    assert "const bool stageStartupMapPreview = softwareFallback" in bootstrap
    assert "frontend-only residency active map CPU/GPU closure deferred" in bootstrap
    assert '"frontend_residency", "common_only_map_deferred"' in bootstrap
    assert "shipmentSounds.Clear()" in bootstrap
    assert "defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)" not in bootstrap[
        bootstrap.index("kDiagnosticStartupMapPreview") - 160:
        bootstrap.index("kDiagnosticStartupMapPreview")
    ]
    backend = (ROOT / "src" / "gfx_vita" / "gfx_vita_backend.cpp").read_text(
        encoding="utf-8"
    )
    assert "vglUseExtraMem(GL_FALSE)" in backend
    assert "kNativeDisplayBufferCount = 2u" in backend
    assert "vglSetDisplayBufferCount" in backend
    assert "vglSetCircularPoolSize" in backend
    assert "legacyImmediate=0" in backend
    assert "struct DynamicViewModelVertex" in backend
    assert "sizeof(DynamicViewModelVertex) == 44" in backend
    assert "std::vector<DynamicViewModelVertex> g_dynamicViewModelUpload" in backend
    assert "BindDynamicViewModelAttributes(vertices)" in backend
    assert "firstVertex\n                               * sizeof(DynamicViewModelVertex)" in backend

    # Keep certification tied to the complete retail-order map table.
    catalog=(MAP_HEADER.parent/'vita_map_catalog.inc').read_text()
    map_rows = re.findall(r'VITA_MAP\((\d+), "[a-z]+", "[A-Z ]+", "MPUI_[A-Z]+", (true|false)\)', catalog)
    assert len(map_rows) == MAP_COUNT
    assert CERTIFIED == {int(index) for index,enabled in map_rows if enabled=='true'}
    print("vita map closure tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
