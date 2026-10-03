#!/usr/bin/env python3
"""Static regression checks for the backend-neutral renderer pass contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / "src" / "gfx" / "gfx_backend.h"
BACKEND = ROOT / "src" / "gfx_vita" / "gfx_vita_backend.cpp"
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"
ASSET_DB = ROOT / "src" / "vita" / "vita_asset_db.cpp"
EFFECT_BUNDLE = ROOT / "src" / "vita" / "vita_effect_bundle.cpp"
EFFECT_HEADER = ROOT / "src" / "vita" / "vita_effect_bundle.h"
MAP_ASSETS = ROOT / "src" / "vita" / "vita_map_assets.h"
VITA_CMAKE = ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt"
CAPTURE_SCRIPT = ROOT / "tools" / "vita" / "capture_map_screenshots.ps1"


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    backend = BACKEND.read_text(encoding="utf-8")
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
    asset_db = ASSET_DB.read_text(encoding="utf-8")
    effect_bundle = EFFECT_BUNDLE.read_text(encoding="utf-8")
    effect_header = EFFECT_HEADER.read_text(encoding="utf-8")
    map_assets = MAP_ASSETS.read_text(encoding="utf-8")
    vita_cmake = VITA_CMAKE.read_text(encoding="utf-8")
    capture_script = CAPTURE_SCRIPT.read_text(encoding="utf-8")

    assert "struct GfxShadowPassState" in header
    assert "GFX_SHADOW_PASS_HAS_LIGHTGRID" in header
    assert "struct GfxEffectBatchState" in header
    assert "GFX_EFFECT_BATCH_WEATHER" in header
    assert "GFX_EFFECT_BATCH_SOURCE_AUTHORED" in header
    assert "GFX_EFFECT_BATCH_SMOKE" in header
    assert "GFX_EFFECT_BATCH_FIRE" in header
    assert "GFX_MAX_EFFECT_PLACEMENTS" in header
    assert "cameraForward" in header
    assert "uint32_t sourceStateBits0 = 0;" in header
    assert "uint32_t sourceStateBits1 = 0;" in header
    assert "uint8_t sourceStateValid = 0;" in header
    assert "GFX_MATERIAL_PASS_SOURCE_UNLIT" in header
    assert "GFX_MATERIAL_PASS_SOURCE_UNLIT_MULTIPLY" in header
    assert "high byte is sun visibility" in header
    assert "void (*SetShadowPass)(const GfxShadowPassState* state);" in header
    assert "void (*SetEffectBatch)(const GfxEffectBatchState* state);" in header
    assert "constexpr uint32_t GFX_MAX_DYNAMIC_LIGHTS = 4;" in header
    assert "GfxPrimaryLightDescriptor dynamicLights[GFX_MAX_DYNAMIC_LIGHTS]" in header

    assert "void SetShadowPass(const GfxShadowPassState* state)" in backend
    assert "void SetEffectBatch(const GfxEffectBatchState* state)" in backend
    assert "shadowPass=%u effectBatches=%u" in backend
    assert "GFX_EFFECT_BATCH_WEATHER | GFX_EFFECT_BATCH_PARTICLE" in backend
    assert "SoftwareSetSamplerState(3u, state->colorSamplerState)" in backend
    assert "DecodeSourceSunVisibility" in backend
    assert "EffectiveSourceStateBits0" in backend
    assert "SourceTechniqueIsUnlit" in backend
    assert "!sourceLightmappedPass && !bakedLighting && !sourceUnlit" in backend
    assert "SoftwareSourceBlendOperation" in backend
    assert "ApplyNativeSourceState" in backend
    assert "void EvaluatePrimaryLightResponse" in backend
    assert "void EvaluateDynamicLightResponse" in backend
    assert "u_dynamic_light_position_radius" in backend
    assert "u_specular_color_scale" in backend
    assert "vec3 primaryLightResponse = vec3(0.0);" in backend
    assert "1.0f + primaryLightResponse[channel]" in backend
    assert "light.cosHalfFovOuter" in backend
    assert "localShadowVisibility" in backend
    assert "DrawIndexedNativeColorPass" in backend
    assert "u_has_color" in backend
    assert "u_alpha_test_enabled" in backend
    assert "precision highp float;" in backend
    assert "reinterpret_cast<uintptr_t>(vertices) + offset" in backend
    assert "for (GLuint attribute = 0u; attribute < 8u; ++attribute)" in backend
    assert "glBlendFuncSeparate" in backend
    assert "glStencilOpSeparate" in backend
    assert "glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE)" in backend
    assert "packedShadowGrid" in backend
    assert "localLightGridSunWeight" in backend
    assert "CreateNativePostResources" in backend
    assert "kNativePostFragmentShader" in backend
    assert "native 960x544 RGBA8 contract" in backend
    assert "glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA" in backend
    assert "GL_RGBA16F" not in backend
    assert "CreateStaticVertexBuffer" in header
    assert "DrawStaticIndexed" in header
    assert "struct StaticWorldVertex" in backend
    assert "static_assert(sizeof(StaticWorldVertex) == 44" in backend
    assert "glBufferData(GL_ARRAY_BUFFER" in backend
    assert "DrawNativeWorld" in bootstrap
    assert "ApplyNativePostEffects" in backend
    assert "vitagl-gxm-source-lit-post" in backend
    assert "DisplayDither8x8" in backend
    assert "source-8x8-blue-noise" in backend
    assert "blue-noise dither" in backend

    assert "gfx->SetShadowPass(&shadows);" in bootstrap
    assert "shadows.lightGridSunWeight = state.lightGridSunWeight;" in bootstrap
    assert "surface.sourceStateBits0 = material ? material->litStateBits0" in bootstrap
    assert "ClassifySourceTechniqueFlags" in bootstrap
    assert "surface.sourceTechniqueFlags0 = ClassifySourceTechniqueFlags(material)" in bootstrap
    assert "pass.techniqueFlags0 = surface.sourceTechniqueFlags0;" in bootstrap
    assert "pass.sourceStateBits0 = surface.sourceStateBits0;" in bootstrap
    assert "Keep the existing 84-byte draw vertex ABI" in bootstrap
    assert "localByte = localLightGridSunWeight >= 0.0f" in bootstrap
    assert "shadowVisibility = 1.0f" in bootstrap
    assert "renderer_primary_light_shadows" in bootstrap
    assert "GfxVita_DrawWeather" in bootstrap
    assert "WeatherPlacement" in bootstrap
    assert "projectedAnchorCount" in backend
    assert "if (authoredState)" in backend
    assert "effect = *authoredState;" in backend

    assert "enum class VitaEffectVisualClass" in effect_header
    assert "VitaEffectVisualClass VitaEffectBundle::VisualClass" in effect_bundle
    assert "ClassifyEffectPath" in effect_bundle
    assert "void StageMapEffectTextures" in bootstrap
    assert "renderer_fx_residency" in bootstrap
    assert "bool DrawAuthoredMapEffects" in bootstrap
    assert "DrawTransientAuthoredEffect" in bootstrap
    assert "CollectSourceFxDynamicLights" in bootstrap
    assert "GFX_EFFECT_BATCH_TAIL" in header
    assert "DrawProjectedEffectQuad" in bootstrap
    assert "SourceFxBuildOrientationAxis" in bootstrap
    assert "SourceFxBuildElementAxis" in bootstrap
    assert "SourceFxVelocityAt" in bootstrap
    assert "SourceFxBuildTailQuad" in bootstrap
    assert "CollectSourceFxModelEntities" in bootstrap
    assert "modelPhysicsFallbacks" in bootstrap
    assert "worldQuad" in bootstrap
    assert "renderer_fx_elements" in bootstrap
    assert "renderer FX lights %s" in bootstrap
    assert "authoredEffectTextures" in bootstrap
    assert "GFX_EFFECT_BATCH_SOURCE_AUTHORED" in bootstrap
    assert "LoadMapEffectImagesFromPackage" in bootstrap
    assert "shipmentEffectImages" in bootstrap
    assert "VitaMapEffectImagesAssetId" in map_assets
    assert "0x4D49070000000000ull" in map_assets
    assert "KISAK_VITA_SMOKE_AUTHORED_FX_EVENT" in vita_cmake
    assert "SmokeAuthoredFxEvent" in capture_script

    # The current multi-map bundle emits VITW v3. Rejecting that version at
    # the generic launch gate leaves the world renderer visible but blocks the
    # real frontend -> DM handoff.
    assert "hasBinaryHeader(VitaAssetType::World, 0x57544956, 3)" in asset_db

    print("vita renderer descriptor tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
