#!/usr/bin/env python3
"""Regression checks for the Vita world-view camera clipping path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"


def main() -> int:
    source = BOOTSTRAP.read_text(encoding="utf-8")
    assert "struct ShipmentCameraPoint" in source
    assert "const auto InterpolateCameraPoint" in source
    assert "const auto ClipDistance" in source
    assert "case 0: return point.z - kNear" in source
    assert "Most Shipment triangles are wholly inside the view" in source
    assert "bool allInside = true" in source
    assert "for (uint32_t plane = 0; plane < 5; ++plane)" in source
    assert "if (allInside)" in source
    assert "uint32_t clippedCount = 0" in source
    assert "std::swap(polygon, clipped)" in source
    assert "point.hasSourceIndex" in source
    assert "surface-local index stream" in source
    assert "projectedSourceIndices" in source
    assert "sourceLightGridSamples" in source
    assert "const auto SampleSourceLightGrid" in source
    assert "sourceSunShadowSamples" in source
    assert "const auto SampleSourceSunShadow" in source
    assert "sourceShadowSamples" in source
    assert "pointShadowValid" in source
    assert "Direct source vertices get their own authored light-grid lookup" in source
    assert "PackSourceLightGridSample" in source
    assert "kLightGridSampleInvalid" in source
    # Keep the assertion tolerant of the current formatted multi-line call;
    # the important contract is that each authored surface range is forwarded
    # with its source vertex/index base and triangle count.
    assert "AppendRange(surfaceIndex," in source
    assert "preview.surfaces[surfaceIndex].firstVertex" in source
    assert "preview.surfaces[surfaceIndex].baseIndex" in source
    assert "preview.surfaces[surfaceIndex].triangleCount" in source
    assert "std::max<uint32_t>(1u, perSurfaceBudget)" in source
    assert "static_cast<uint32_t>(firstVertex)" in source
    assert "activeWeapon->adsZoomFov" in source
    assert "float viewFov = 65.0f" in source
    assert "player->weaponPosFrac" in source
    assert "viewFov = 65.0f - (65.0f - adsFov) * adsFraction" in source
    assert "fWeaponPosFrac" in source
    assert "struct VitaProjection" in source
    assert "tanHalfFovY = std::max(0.0001f, tanHalfFov * 0.75f)" in source
    assert "float EncodeCameraDepth(float cameraZ, float nearPlane)" in source
    assert "return std::clamp(1.0f - nearPlane / safeZ, 0.0f, 1.0f);" in source
    assert "EncodeCameraDepth(safeZ, kNear)" in source
    assert "*outDepth = EncodeCameraDepth(cameraZ, kNear);" in source
    assert "BuildPlayerView(shipmentPreview, *player," in source
    assert "leaves room for generated clip vertices" in source
    assert "projectedVertices.size() > UINT16_MAX" in source
    assert "if (cameraZ < kNear)" not in source
    # Large scalar maps are submitted synchronously in conservative chunks.
    # Keeping the target below the allocator's next growth boundary prevents
    # the 32k -> 64k guest-vector allocation that previously aborted Countdown
    # after its first 1,024 BSP surfaces.
    # The stream path uses a 4k target so projection never crosses the next
    # guest-vector allocation boundary while a chunk is being submitted.
    assert "kStreamChunkTargetVertices = 4096u" in source
    assert "currentChunkVertices >= chunkVertexLimit" in source
    assert "chunkVertexLimit = streamGfx" in source
    assert "kStreamChunkTargetVertices" in source
    # Static XModels use the same five-plane scalar clipping contract as the
    # BSP.  Regressing to the old whole-triangle near-plane reject recreates
    # holes on close containers/foliage in the software oracle.
    assert "struct ClippedStaticPoint" in source
    assert "const auto StaticClipDistance" in source
    assert "const auto InterpolateStaticPoint" in source
    assert "staticModelClippedVertices" in source
    assert "staticModelClippedSourceIndices" in source
    assert "surfaceClipped" in source
    assert "for (uint32_t plane = 0; plane < 5; ++plane)" in source
    assert "The old reject" in source
    assert "close crate/foliage card" in source
    print("vita renderer projection tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
