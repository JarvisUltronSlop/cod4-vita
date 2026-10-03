#!/usr/bin/env python3
"""Regression checks for bounded emulator world-texture staging."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"
BACKEND = ROOT / "src" / "gfx_vita" / "gfx_vita_backend.cpp"


def main() -> int:
    source = BOOTSTRAP.read_text(encoding="utf-8")
    backend = BACKEND.read_text(encoding="utf-8")
    assert "const bool softwareFallback = GfxVita_IsSoftwareFallback();" in source
    assert "hugeMap ? 32u : 64u" in source
    assert "12u * 1024u * 1024u" in source
    assert "high-fidelity diagnostic mode" in source
    assert '"sp_ambush_ft"' in source
    assert "Prebind the authored sky face" in source
    assert "static_cast<size_t>(residentWidth) * residentHeight" in source
    assert "sizeof(uint32_t)" in source
    assert "decoding the complete retail set" in source
    assert "authored atlas as well" in source
    assert "LoadMenuBackgroundTexture" in source
    assert "FindShipmentSkyTexture" in source
    assert "DrawShipmentSky" in source
    assert "ReleaseScalarSpecularPayloads" in source
    assert "ReleaseScalarWorldNormalPayloads" in source
    assert "released pre-model specular source bytes" in source
    assert "released pre-world normal source bytes" in source
    # Frontend payloads must survive the scalar world/lightmap compaction
    # until the authored background, button atlas, and font are uploaded.
    assert "&shipmentEntities, true, true);" in source
    assert "if (batch.mode == GfxSurfaceMode::Sky)" in source
    assert "if (GfxVita_IsSoftwareFallback())" in source
    assert "staged bounded world/loading/HUD textures" in source
    # Cubemaps do not enter the 2D resample branch, but huge-map scalar
    # dimensions and the six-face residency estimate must still apply to
    # their authored sky payloads.
    assert "const bool hugeMap = (flags & GFX_VITA_TEXTURE_FLAG_SCALAR_HUGE_MAP) != 0;" in backend
    assert "const size_t faceCount = softwareFallback && cubeMap ? 6u : 1u;" in source
    # A high-precision destination must not be fed by the compact RGBA4444
    # thumbnail first; that would reintroduce the sky/material banding this
    # path is intended to diagnose.
    # The source thumbnail bound is map/asset-class dependent now (large
    # maps and lightmaps use tighter authored limits), but it must still feed
    # the decoder at the requested precision before the world resample.
    assert "sourceThumbnailLimit" in backend
    assert "&source, sourceThumbnailLimit," in backend
    assert "SoftwareTextureTexel(source" in backend
    assert "&decodedFace, thumbnailLimit, true" in backend
    assert "SoftwareTextureTexel(decodedFace" in backend
    print("vita texture budget tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
