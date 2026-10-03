#!/usr/bin/env python3
"""Regression checks for the hardware Vita dual-texture world path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"
BACKEND = ROOT / "src" / "gfx_vita" / "gfx_vita_backend.cpp"
API = ROOT / "src" / "gfx" / "gfx_backend.h"
IMAGE_REPAIR = ROOT / "tools" / "vita_asset_prep" / "repair_map_image_closures.py"


def main() -> int:
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
    backend = BACKEND.read_text(encoding="utf-8")
    api = API.read_text(encoding="utf-8")
    image_repair = IMAGE_REPAIR.read_text(encoding="utf-8")
    assert "MODEL_COLOR_IMAGE_FAMILY = 0x4D49060000000000" in image_repair
    assert "CRITICAL_MAP_WORLD" in image_repair
    assert "COLOR_CLOSURE_MAX_DATA_BYTES = MAX_DATA_BYTES + 512 * 1024" in image_repair
    assert "existing validated companion preserved" in image_repair
    # Rain is part of the source image closure on Farm/Downpour and Cargo
    # Ship/Wet Work; keep the exact IWI aliases in the repaired map stream.
    assert "weather_priority_names" in image_repair
    assert '["rain_spray", "wind_rain_atlas"]' in image_repair
    assert "BUILTIN_IMAGE_RECORDS" in image_repair
    assert '"default": (4, 1, 1, 1, 1, 21' in image_repair
    assert "GfxTextureHandle lightmapTexture{0};" in bootstrap
    assert "LoadShipmentLightmaps" in bootstrap
    assert '"*lightmap%u_%s"' in bootstrap
    # Lightmap payloads are released after the first upload on the scalar
    # path. The cache lookup must therefore happen before the source-byte
    # validation, otherwise every later BSP surface loses its atlas handle.
    lightmap_stage = bootstrap[bootstrap.index("const auto Stage = [&](uint32_t lightmapIndex"):
                                bootstrap.index("for (ShipmentSurface& surface", bootstrap.index("const auto Stage = [&](uint32_t lightmapIndex"))]
    assert lightmap_stage.index("if (cached == cache.end())") < lightmap_stage.index(
        "const auto* image = images->Find(imageName)")
    assert "images->ReleasePayload(image->name.c_str())" in lightmap_stage
    # KisakCOD binds the primary lightmap with sampler state 0x62 (linear
    # min/mag filtering). Keep the scalar diagnostic on the same texel-center
    # bilinear lookup instead of letting it regress to a nearest grid.
    assert "SamplePrimaryLightmap" in bootstrap
    # The scalar diagnostic must follow the retail lm_* shader's dual-map
    # equation rather than silently regressing to a hand-tuned primary-only
    # floor when the authored secondary atlas/normal payload is present.
    assert "SampleSecondaryLightmap" in bootstrap
    assert "ComputeSourceStaticLightmapLighting" in bootstrap
    assert "RestoreSourceWorldLighting" in bootstrap
    # The full source equation is now evaluated in the backend's fragment
    # loop, while this retained helper remains the decode reference for the
    # source-lighting tests.
    assert "g_softwareLightmapSamplerFlags" in backend
    assert "CanonicalizeA8R8G8B8" in backend
    assert "value = CanonicalizeA8R8G8B8(value)" in backend
    assert "GL_BGRA" in backend
    assert "authored DXT block already contains the full 8-bit decoded" in backend
    assert "logicalShift[4] = {16u, 8u, 0u, 24u}" in bootstrap
    assert "const bool scalarLightmap = image->format == 50u" in bootstrap
    assert "GFX_VITA_TEXTURE_FLAG_SCALAR_HIGH_PRECISION" in bootstrap
    assert "secondaryRgb" in bootstrap
    assert "sunLightColor[component] * sunDot" in bootstrap
    # Keep the source specular semantic alive through scalar staging.  The
    # renderer samples the authored mask into the pointer-free BSP stream
    # before releasing compressed payloads; the view-dependent highlight can
    # then be added without inventing a Vita-side material or texture.
    assert "std::vector<std::array<uint8_t, 4>> sourceSpecular" in bootstrap
    assert "std::lround" in bootstrap
    assert "ApplySourceSpecularMasks" in bootstrap
    assert "renderer_specular_masks" in bootstrap
    assert "material->specularMap" in bootstrap
    assert "lightmapFlags & 0x06u" in bootstrap
    assert "ApplySourceSunSpecular" in bootstrap
    assert "reflectionSun" in bootstrap
    assert "0.99925f" in bootstrap
    assert "9.3775177f" in bootstrap
    assert "crossesNearPlane" in bootstrap
    assert "source.indices.data() + surface.indexStart" in bootstrap
    assert "BuildClippedSurface" in bootstrap
    assert "const float texelX = clampedU * static_cast<float>(image.width) - 0.5f" in bootstrap
    assert "const float top = Sample(x0, y0)" in bootstrap
    assert "surface.lightmapTexture.value != 0" in bootstrap
    assert "GfxTextureHandle secondaryLightmapTexture{0};" in bootstrap
    assert "*lightmap%u_%s" in bootstrap
    assert "boundSecondarySurfaces" in bootstrap
    assert "DrawIndexedLightmapped" in bootstrap
    assert "DrawIndexedLightmapped" in backend
    assert "DrawIndexedLightmapped is a complete source lit pass" in backend
    assert "glVertexPointer(3, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex))" in backend
    assert "glColorPointer(4, GL_FLOAT, static_cast<GLsizei>(sizeof(GfxDrawVertex))" in backend
    assert "const auto BindLightmap" in backend
    assert "GL_TEXTURE2" in backend
    assert "vertices->lightmapCoord" in backend
    assert "DrawIndexedLightmapped" in api
    assert "GfxSurfaceMode" in api
    assert "SetSurfaceMode" in api
    assert "SetFog" in api
    assert "SetFog(bool enabled" in backend
    assert "GL_FOG_DENSITY" in backend
    # The authored sky is an infinite cubemap pass, so fog must remain on the
    # world/prop batches but be disabled while sky pixels are submitted.  This
    # guards the regression where the far-plane exponential factor flattened
    # Shipment's clouds into an opaque grey field.
    assert "g_softwareSurfaceMode == GfxSurfaceMode::Sky" in backend
    assert "case GfxSurfaceMode::Sky:" in backend
    assert "glDisable(GL_FOG)" in backend
    assert "double-darken every lit surface" in bootstrap
    assert "SurfaceBatchOrder" in bootstrap
    assert "std::stable_sort(batches.begin(), batches.end()" in bootstrap
    assert "ClassifyWorldSurface" in bootstrap
    # wc_sky is an authored six-face cube pass. Its retail GfxStateBits are
    # opaque, so the dedicated sky classification must happen before the
    # generic VITL lit-state branch or a state-bit census can demote it to a
    # flat 2D material and reintroduce the olive fog field/seam regression.
    sky_classification = bootstrap.index('technique == "wc_sky"')
    lit_state_branch = bootstrap.index("if (material->litStateValid)")
    assert sky_classification < lit_state_branch
    assert "dedicated cube-sampler pass" in bootstrap
    assert "GfxSurfaceMode::AlphaTest" in backend
    # KisakCOD's authored b-family materials combine a cutout test with
    # source-alpha blending. Keep the exact comparison function (GT0/LT128/
    # GE128) explicit in both the Vita API and the scalar diagnostic path.
    assert "enum class GfxAlphaTestFunc" in api
    assert "AlphaTestBlend = 6" in api
    assert "void (*SetAlphaTestFunc)(GfxAlphaTestFunc function);" in api
    assert "ClassifyWorldAlphaTestFunc" in bootstrap
    assert "kAlphaTestDisable" in bootstrap
    assert "GfxSurfaceMode::AlphaTestBlend" in bootstrap
    assert "surface.alphaTestFunc" in bootstrap
    assert "SetAlphaTestFunc(batch.alphaTestFunc)" in bootstrap
    assert "GfxAlphaTestFunc g_softwareAlphaTestFunc" in backend
    assert "SoftwareAlphaTestPass" in backend
    assert "case GfxSurfaceMode::AlphaTestBlend" in backend
    assert "void SetAlphaTestFunc(GfxAlphaTestFunc function)" in backend
    assert "GL_GEQUAL" in backend
    assert "GfxSurfaceMode::Additive" in backend
    # Material_SetMaterialDrawRegion marks shadow-only geometry as region 3;
    # the Vita camera pass must preserve KisakCOD's omission instead of
    # drawing those cards into the color buffer.
    assert "uint8_t cameraRegion = 0;" in bootstrap
    assert "surface.cameraRegion = material ? material->cameraRegion : 0u" in bootstrap
    assert "surface.cameraRegion == 3u" in bootstrap
    assert "hasShadowOnly" in bootstrap
    assert "surface.lightmapSamplerFlags = material" in bootstrap
    assert "uint8_t colorSamplerState = 0;" in bootstrap
    assert "surface.colorSamplerState = material" in bootstrap
    assert "value.colorSamplerState == colorSamplerState" in bootstrap
    assert "gfx->SetSamplerState(0u, batch.colorSamplerState)" in bootstrap
    assert "secondaryLightmapSurfaces" in bootstrap
    # Keep an auditable source-material census next to the residency trace so
    # the normal/specular/detail shader work is driven by the visible Shipment
    # technique families instead of a guessed material list.
    assert "renderer_material_census" in bootstrap
    assert "normalMapPayloads" in bootstrap
    assert "specularMapPayloads" in bootstrap
    # The scalar oracle retains a bounded, high-precision source normal set
    # and binds it only on lightmapped world surfaces; non-lightmapped cards
    # continue using the one-time source bridge to avoid double application.
    assert "normalSemanticTextures" in bootstrap
    assert "StageScalarSemantic" in bootstrap
    assert "GFX_VITA_TEXTURE_FLAG_SCALAR_HIGH_PRECISION" in bootstrap
    assert "normalBoundSurfaces" in bootstrap
    # The complete world normal family is streamed after startup compaction.
    # Bound aliases must reach the scalar material pass as real handles, while
    # over-budget aliases retain the source bridge as their one-time fallback.
    assert "canStageScalarTextures" in bootstrap
    assert "BindAlias" in bootstrap
    assert "normalBudget" in bootstrap
    assert "renderer_scalar_normal_companion" in bootstrap
    assert "surface.normalTexture.value != 0" in bootstrap
    assert "specularSemanticTextures" in bootstrap
    assert "specularBoundSurfaces" in bootstrap
    assert "activeSurfaceHasSemanticSpecular" in bootstrap
    assert "specularBudget" in bootstrap
    assert "renderer_specular_companion" in bootstrap
    assert "surface.specularTexture = handle" in bootstrap
    assert "postStageNormals" in bootstrap
    assert "u_camera_position" in backend
    assert "cameraPosition" in backend
    assert "secondaryTopAlpha" in backend
    assert "secondaryBottomAlpha" in backend
    # Retail lm_* uses the positive authored sun direction and has no
    # readability floor or camera-grid scale on static BSP lightmaps.
    assert "sourceLightmapNormalXY" in backend
    assert "sourceLightmapNormal[0]" in backend
    assert "* g_frameLighting.sunDirection[0]" in backend
    assert "staticSunScale" not in backend
    assert "0.36f + 0.64f" not in backend
    assert "0.55f + 0.45f" not in backend
    assert "activeSurfaceHasAuthoredLightmap" in bootstrap
    assert "!activeSurfaceHasAuthoredLightmap" in bootstrap
    assert "staticLight" in backend
    assert "RecordFrameLightmapTelemetry" in backend
    assert "lightmap telemetry frame=%u" in backend
    assert "u_lightmap_secondary, topCoord" in backend
    assert "familyAlphaTest" in bootstrap
    assert "CountTechniqueFamily" in bootstrap
    # VITL v4 carries the source MaterialPass lightmap sampler bits and the
    # first source lit-pass GfxStateBits pair. Keep the pointer-free fields
    # present before a backend starts consuming those authored state rules.
    material_header = (ROOT / "src" / "vita" / "vita_material_bundle.h").read_text(
        encoding="utf-8")
    material_reader = (ROOT / "src" / "vita" / "vita_material_bundle.cpp").read_text(
        encoding="utf-8")
    registry = (ROOT / "src" / "database" / "db_registry.cpp").read_text(
        encoding="utf-8")
    assert "lightmapSamplerFlags" in material_header
    assert "kVersionV4 = 4" in material_reader
    assert "kV4RecordHeaderBytes = 42" in material_reader
    assert "material.lightmapSamplerFlags = data[offset + 32]" in material_reader
    assert "material.litStateBits0 = ReadU32(data.data() + offset + 33)" in material_reader
    assert "material.litStateBits1 = ReadU32(data.data() + offset + 37)" in material_reader
    assert "material.litStateValid = data[offset + 41]" in material_reader
    assert "DB_VitaMaterialLightmapSamplerFlags" in registry
    assert "DB_VitaMaterialLitStateBits" in registry
    assert "const uint32_t version = 4;" in registry
    assert "SetSamplerState(uint32_t textureUnit, uint8_t samplerState)" in backend
    assert "GL_TEXTURE_WRAP_S" in backend
    assert "SetSamplerState(unit, 0x62u)" in backend
    assert "void (*SetSamplerState)(uint32_t textureUnit, uint8_t samplerState);" in api
    # Cubemap taps cross authored face boundaries in the retail sky shader;
    # keeping those edge taps source-seam-aware prevents a scalar-only seam
    # while preserving the six converted faces.
    assert "SampleSeamAwareTap" in backend
    assert "DirectionForFace" in backend
    assert "D3D cubemap sampling is seam-aware" in backend
    # Preserve KisakCOD's authored GfxStateBits cull policy across both the
    # VitaGL and scalar diagnostic paths.  The source uses D3D's clockwise
    # front-face convention, so the backend must expose explicit Back/Front
    # modes instead of silently treating every Shipment surface as two-sided.
    assert "enum class GfxCullMode" in api
    assert "void (*SetCullMode)(GfxCullMode mode);" in api
    assert "void SetCullMode(GfxCullMode mode)" in backend
    assert "glFrontFace(GL_CW)" in backend
    assert "glCullFace(GL_BACK)" in backend
    assert "glCullFace(GL_FRONT)" in backend
    assert "GfxCullMode ClassifyWorldCull" in bootstrap
    assert "material->litStateBits0 & 0xC000u" in bootstrap
    assert "surface.cullMode = ClassifyWorldCull(material)" in bootstrap
    assert "value.cullMode == cullMode" in bootstrap
    assert "gfx->SetCullMode(batch.cullMode)" in bootstrap
    assert "g_softwareCullMode == GfxCullMode::Back" in backend
    assert "g_softwareCullMode == GfxCullMode::Front" in backend
    # Static XModels must carry the authored tangent frame and material
    # normalMap response into the same fixed-function colour bridge as BSP
    # surfaces; otherwise Shipment's containers/props remain diffuse-only.
    assert "staticModelNormalMappedVertices" in bootstrap
    assert "destination.tangent[0] = worldTangent[0]" in bootstrap
    assert "ApplySourceNormalMapSample" in bootstrap
    assert "normalMappedModelSurfaces" in bootstrap
    assert "ReleaseScalarSourcePayloads" in bootstrap
    assert "renderer_model_normal_payloads" in bootstrap
    assert "InterpolateQuadUv" in backend
    assert "all four authored" in backend
    # The grid sampler must retain KisakCOD's source corner numbering and
    # needsTrace sight test; a generic XYZ trilinear lookup changes authored
    # light selection around clipped/container corners.
    assert "ShipmentLightGridEntryQuad" in bootstrap
    assert "R_GetLightGridSampleEntryQuad" in bootstrap
    assert "const uint8_t* rleData = rowData + 12u" in bootstrap
    assert "const uint32_t fullRecordBytes = zCount > 255u ? 4u : 3u" in bootstrap
    assert "corner & 4" in bootstrap
    assert "corner & 2" in bootstrap
    assert "corner & 1" in bootstrap
    assert "needsTrace" in bootstrap
    assert "TraceSphere(position, gridPoint, 0.0f, 8193u" in bootstrap
    assert "ShipmentCollision* collision" in bootstrap
    # Match R_ExtrapolateLightingAtPoint when a sparse row has no populated
    # corner: use the authored default grid record and preserve its sun weight,
    # rather than the final missing-light diagnostic color.
    assert "uint32_t defaultGridEntry = 1u" in bootstrap
    assert "const bool hasSourceDefault = defaultGridEntry < colorCount" in bootstrap
    assert "result.sunWeight = hasSourceDefault ? 1.0f : 0.0f" in bootstrap
    assert "std::min(grid.sunPrimaryLightIndex, 255u)" in bootstrap
    # CM_BoxSightTrace's source sight path is brush/BSP based. Keep the
    # optional collision mesh from globally shadowing unrelated leaves until
    # the VITC stream carries the source leaf/content partition.
    collision_header = (ROOT / "src" / "vita" / "vita_shipment_collision.h").read_text(
        encoding="utf-8")
    collision_source = (ROOT / "src" / "vita" / "vita_shipment_collision.cpp").read_text(
        encoding="utf-8")
    assert "TraceLineBrushesOnly" in collision_header
    assert "return TraceSphere(start, end, 0.0f, contentsMask, result);" in collision_source
    assert "collision->TraceLineBrushesOnly" in bootstrap
    # Streaming is a buffer-lifetime optimization only. Both scalar and
    # VitaGL/GXM must consume the same collision-backed source lightgrid and
    # sun-shadow samples for large maps.
    assert "const bool sourceSurfaceLighting = preview.lightGrid.valid" in bootstrap
    assert "constexpr size_t kPerVertexLightGridVertexLimit = 260000u" in bootstrap
    assert "constexpr size_t kPerVertexShadowVertexLimit = 260000u" in bootstrap
    assert "const bool sourcePerVertexLightGrid = sourceSurfaceLighting" in bootstrap
    assert "const bool sourcePerVertexShadow = sourceSurfaceLighting" in bootstrap
    assert "sourcePerVertexLightGrid && point.hasSourceIndex" in bootstrap
    assert "const ShipmentSunShadowSample sourceShadow = sourcePerVertexShadow" in bootstrap
    assert "BuildBrushBroadphase" in collision_source
    assert "ProductWithinLimit" in collision_source
    assert "kBrushGridCellSize = 256.0f" in collision_source
    assert "kBrushGridMaxQueryCells" in collision_source
    assert "full walk; the broadphase is an optimization" in collision_source
    assert "BroadphaseCellCount" in collision_header
    assert "renderer_sky_binding" in bootstrap
    assert "renderer_lightgrid_frame" in bootstrap
    assert "VitaPrimaryLightAffectsPoint" in bootstrap
    assert "primaryVisibleWeight" in bootstrap
    assert "primaryOccludedWeight" in bootstrap
    assert "primaryLightWeight" in bootstrap
    # R_GetLightingAtPoint coalesces duplicate color records and performs the
    # authored 8.8 fixed-point blend before packing the 56 directional texels.
    # A float average here changes model-light colours at sparse/interior
    # boundaries even though the underlying source records are identical.
    assert "coalesces corners" in bootstrap
    assert "std::array<uint16_t, 8u> fixedPointWeights" in bootstrap
    assert "fixedPointWeightSum" in bootstrap
    assert "R_FixedPointBlendLightGridColors" in bootstrap
    assert "sourcePrimaryLightWeight" in bootstrap
    # Local source lightgrid colour is applied during projection. Both
    # backends must avoid multiplying the camera-global grid a second time.
    assert "vec3 frameGrid = hasLocalLightGridSunWeight" in backend
    assert "? vec3(1.0)" in backend
    assert "const float grid = hasLocalLightGridSunWeight" in backend
    assert "? 1.0f" in backend
    print("vita lightmap renderer tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
