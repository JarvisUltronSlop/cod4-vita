#!/usr/bin/env python3
"""Regression checks for Vita3K diagnostics and accelerated qualification."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"
DB_REGISTRY = ROOT / "src" / "database" / "db_registry.cpp"
BACKEND = ROOT / "src" / "gfx_vita" / "gfx_vita_backend.cpp"
RUNNER = ROOT / "tools" / "vita" / "run_guarded_vita3k.ps1"


def main() -> int:
    source = BOOTSTRAP.read_text(encoding="utf-8")
    database_source = DB_REGISTRY.read_text(encoding="utf-8")
    backend = BACKEND.read_text(encoding="utf-8")
    runner = RUNNER.read_text(encoding="utf-8")
    assert "complete projected BSP stream" in source
    assert "kSoftwareViewRefreshMsec = 250" in source
    assert "softwareViewValid" in source
    assert '"renderer_projection", "software_source_fullstream"' in source
    assert '"renderer_visibility", detail' in source
    assert '"renderer_textures", detail' in source
    assert "drawIndexCount -= drawIndexCount % 3u" in source
    assert "viewFov);" in source
    assert "kSoftwareWorldTriangleBudget = UINT32_MAX" in source
    assert "const bool visibleSampling = triangleBudget != UINT32_MAX" in source
    # Bounded probes now derive their per-surface count from the requested
    # global budget.  Keep the assertion tied to that source-derived cap
    # instead of the retired fixed 24-triangle diagnostic allowance.
    assert "const uint32_t probeCount = visibleSampling" in source
    assert "std::min<uint32_t>(triangleCount," in source
    assert "if (!GfxVita_IsSoftwareFallback())" in source
    assert "software fallback: staged bounded world/loading/HUD textures" in source
    assert "software fallback: staged menu/font textures" in source
    assert "audio init deferred until first frame" in source
    assert "audioReady = !GfxVita_IsSoftwareFallback()" in source
    assert "KISAK_VITA_VITA3K_NO_AUDIO" in source
    assert "reason=vita3k-qualification-bypass" in source
    assert "audioGameplayMarked" in source
    assert "kVitaWeaponGunModelCount" in source
    assert "weapon.worldClipModelName" in source
    assert "element.elemType != 5u" in source
    assert "RetainModelName(visual.c_str())" in source
    assert "diagnostic rasterizer" in source
    assert "kSoftwareRasterPixelBudget = 64u * 1024u * 1024u" in backend
    assert "kSoftwareRasterStep = 1" in backend
    assert "void SoftwareStoreBlock" in backend
    assert "buffer immediately so the last composited HUD/viewmodel pass" in backend
    assert "SCE_DISPLAY_SETBUF_IMMEDIATE" in backend
    assert "g_softwareDisplayFramebuffer" in backend
    assert "framebuffer static double-buffer" in backend
    assert "MaybeDumpSoftwareFrame" in backend
    assert "software-frame.ppm" in backend
    assert "const auto Bilinear" in backend
    assert "const bool clampAtlas" in backend
    assert "texelX = u * static_cast<float>(texture.width) - 0.5f" in backend
    assert "edge-clamped filtering" in backend
    assert "perspectiveWorld" in backend
    assert "void ResetExtraFixedFunctionTextureUnits()" in backend
    assert backend.count("ResetExtraFixedFunctionTextureUnits();") >= 3
    # The fidelity-first model pass derives one source-index budget from the
    # authored KisakCOD entity/model closure, so scalar diagnostics and the
    # accelerated Vita path select the same complete Shipment dressing.
    assert "uint64_t sourceIndexBudget = 0" in source
    assert "sourceIndexBudget + static_cast<uint64_t>(model->indices.size())" in source
    assert "uint32_t remainingIndices = static_cast<uint32_t>(sourceIndexBudget)" in source
    assert "drawnModels >= 512u" not in source
    assert "Do not manufacture a prop colour" in source
    assert "const float entityPitch = entity.hasAngles" in source
    # Source-derived static-model lighting is expressed as the authored
    # ambient/diffuse/sun sum with a bounded finite clamp; keep the regression
    # check tied to that equation rather than a fixed blend scalar.
    assert "factors[component] = std::clamp(factor, 0.35f, 1.35f)" in source
    # Frame/lightgrid response belongs to unlightmapped/model materials; the
    # authored BSP primary/secondary lightmaps must remain the static world
    # lighting source in both backend implementations.
    assert "const bool sourceLightmappedPass = usePrimaryLightmap" in backend
    assert "if (!sourceLightmappedPass && !bakedLighting)" in backend
    # Coarse scalar shading may reuse one fragment colour for a small block,
    # but depth acceptance/write must remain per destination pixel or a nearer
    # wall can overwrite a doorway/interior across the whole block.
    assert "depth, g_depth[pixel], g_materialPass" in backend
    assert "g_depth[pixel] = depth" in backend
    assert "factors[channel] = std::clamp(response, 0.35f, 1.35f)" in backend
    assert "lit *= clamp(response, vec3(0.35), vec3(1.35));" in backend
    assert "0.72f + 0.28f * response" not in backend
    # Model-light samples are source-authored directional responses. The
    # scalar/native bridge must preserve the complete 56-texel light shell and
    # use the same transformed-normal lookup for hands, weapons, and props.
    assert "std::array<uint8_t, 56u * 3u> directionalColors" in source
    assert "bool EvaluateSourceModelLightGrid" in source
    assert "bool EvaluateSourceModelLightResponse" in source
    assert "2.0f * modelLight + alpha * sunColor * sunDot" in source
    assert "static constexpr uint8_t kShellIndex[4][4][4]" in source
    assert "EvaluateSourceModelLightGridResponse" in source
    assert "AddLeanToPosition(viewModelLightingOrigin" in source
    assert "pass.techniqueFlags0 = GFX_MATERIAL_PASS_BAKED_LIGHTING" in source
    assert "intersectsScreen" in source
    assert "softwareFallback && !intersectsScreen" in source
    assert "const uint32_t scalarCubeWidth = hugeMap ? 128u : 512u" in source
    assert "const uint32_t scalarCubeHeight = hugeMap ? 256u : 512u" in source
    assert "12u * 1024u * 1024u" in source
    assert '"sp_ambush_ft"' in source
    assert "DrawShipmentSky" in source
    assert "GfxVita_DrawSkyCubemap" in source
    assert "GFX_VITA_TEXTURE_FLAG_SCALAR_HIGH_PRECISION" in source
    # Generated reflection cubemaps are kept source-complete in the package,
    # but their compressed chunk is deferred on the scalar startup path and
    # appended only after native backend selection.
    assert "AppendMapReflectionImagesFromPackage" in source
    assert "scalar reflection image bundle deferred" in source
    assert "native reflection image bundle appended" in source
    assert "LoadMetadata(database, imageAssetId)" in source
    assert "payloadMode=%s" in source
    assert ": images->Load(database, imageAssetId);" in source
    assert "&& images->Load(database, imageAssetId,\n" not in source
    assert "kGradientAnchors" in source
    assert "Gather usage first" in source
    assert "left.uses > right.uses" in source
    assert "kScalarShipmentPriority" in source
    assert '"me_cargocontainergreen_ext_wall_col"' in source
    assert '"me_cargocontainerwhite_ext_wall_col"' in source
    assert '"me_asphalt_01"' in source
    assert '"ch_ground_gravel02_col"' in source
    assert '"body_wall1_col"' in source
    assert '"wall_stain3_col"' in source
    assert "g_softwareRasterPixels = 0" in backend
    assert "g_softwareDepthTest = true" in backend
    assert "g_softwareDepthWrite = true" in backend
    assert "g_softwareSurfaceMode = GfxSurfaceMode::Opaque" in backend
    assert "g_softwareSurfaceMode = mode" in backend
    assert "GfxSurfaceMode::Overlay" in backend
    assert "const bool axisAlignedSky = g_softwareSurfaceMode == GfxSurfaceMode::Sky;" in backend
    assert "void ResetVitaGlMatrices()" in backend
    assert "ResetVitaGlMatrices();" in backend
    assert "GfxSurfaceMode::Additive" in backend
    assert "SoftwareDrawSkyCubemap" in backend
    assert "GL_TEXTURE_CUBE_MAP" in backend
    assert "IMG_FLAG_CUBEMAP" in backend
    assert "uint32_t scalarWidth = width" in backend
    assert "FitTextureDimensions" in backend
    assert "FitTextureDimensions(width, height, scalarWorldLimit" in backend
    assert "aspect-preserving image" in backend
    assert "std::vector<uint32_t> highPrecisionPixels" in backend
    assert "GFX_VITA_TEXTURE_FLAG_SCALAR_HIGH_PRECISION" in backend
    assert "DecodeDxtBlockRgba8888" in backend
    assert "highPrecision ? sizeof(uint32_t) : sizeof(uint16_t)" in backend
    assert "smooth cloud, fog, and painted gradients" in backend
    assert "faceTexels" in backend
    assert "Resample the authored face at texel centers" in backend
    assert "second, visible stair-step" in backend
    assert "GfxSurfaceMode::AlphaTest" in backend
    assert "SoftwareSetSurfaceMode" in backend
    assert "trianglePixels > kSoftwareRasterPixelBudget - g_softwareRasterPixels" in backend
    assert "Long, thin BSP triangles" in backend
    assert "const int rasterStep = (boundsWidth <= 18 || boundsHeight <= 18)" in backend
    assert "0.5f * rasterStep" in backend
    assert "const float corners[4][2]" in backend
    assert "maxA < 0.0f || maxB < 0.0f || maxC < 0.0f" in backend
    assert "wa = std::max(0.0f, wa)" in backend
    assert "DXT1 uses color index 3 as a transparent texel" in backend
    assert "DXT3/DXT5 retain a four-colour RGB palette" in backend
    assert "const bool dxt1 = format == 0x31545844u" in backend
    assert "const uint8_t* colorBlock = dxt1 ? source : source + 8" in backend
    assert "const uint8_t* alpha = source" in backend
    assert "const uint8_t decodedAlpha" in backend
    assert "silently discarded those UVs" in backend
    assert "vertices[0].texCoord[channel]" in backend
    assert "InterpolateQuadUv" in backend
    assert "all four authored" in backend
    assert "Menus are authored alpha-blended UI" in source
    begin_frame = backend.split("void SoftwareBeginFrame()", 1)[1].split(
        "void SoftwareEndFrame()", 1
    )[0]
    assert "sceDisplayWaitVblankStart" not in begin_frame
    assert "vblank wait" in begin_frame
    assert "SetSurfaceMode(GfxSurfaceMode::Overlay)" in source
    assert "Get-Process -Name Vita3K" in runner
    assert "$ErrorActionPreference = 'Stop'" in runner
    assert "Local\\Cod4Vita-Vita3K" in runner
    assert "WaitOne(0)" in runner
    assert "refusing a concurrent launch" in runner
    assert "installed-title runs must omit -Console" in runner
    assert "ColorSurfaceDebug" in runner
    assert "if ($ColorSurfaceDebug -and -not $Console)" in runner
    assert "CaptureSoftwareFrame" in runner
    assert "RequireAccelerated" in runner
    assert "AllowSoftwareFallback" in runner
    assert "CopyRuntimeTrace" in runner
    assert "PackagePath" in runner
    assert "ModelSemanticPackagePath" in runner
    assert "app0:data aligned" in runner
    assert "shipment-db-reference.kvap" in runner
    assert "shipment-db-model-semantics.kvap" in runner
    assert "INSTALLED_MODEL_SEMANTIC_PACKAGE_SHA256" in runner
    assert "INSTALLED_PACKAGE_SHA256" in runner
    assert "TraceOutputDirectory" in runner
    assert "guestTraceRoot" in runner
    assert "preceding scalar" in runner
    assert "shipment-trace.jsonl" in runner
    assert "script-stages.jsonl" in runner
    assert "software-frame.ppm" in runner
    assert "if ($CaptureSoftwareFrame)" in runner
    assert "RUNTIME_TRACE_" in runner
    assert "$acceleratedQualification = $RequireAccelerated -or -not $AllowSoftwareFallback" in runner
    assert "mutually exclusive" in runner
    assert "libshacccg.suprx" in runner
    assert "refusing a scalar fallback launch" in runner
    assert "capture.req" in runner
    assert "resolves --self relative to app0:" in runner
    assert "-S', 'eboot.bin'" in runner
    assert "Installed Vita3K title directory is missing" in runner
    assert "vita3k-$TitleId-eboot.bin.backup" in runner
    assert "INSTALLED_SELF_RESTORED=1" in runner
    assert "installedSelfExisted" in runner
    assert "function Restore-GuardedFile" in runner
    assert "Move-Item -LiteralPath $backupFull -Destination $targetFull -Force" in runner
    assert "function Backup-GuardedFile" in runner
    assert "Move-Item -LiteralPath $sourceFull -Destination $backupFull -Force" in runner
    assert "-WindowStyle Hidden" in runner
    assert "refusing a second instance" in runner
    assert "Stop-Process -Id $process.Id" in runner
    assert "PokeMMO" in runner
    assert "DrawSoftwareShipmentBackdrop" in source
    assert "fallback-only presentation scaffolding" in source
    assert "static constexpr float kMiddle[] = {0.28f, 0.22f, 0.18f, 1.0f}" in source
    assert source.count("GfxVita_BeginOverlayPass") >= 2
    assert "void GfxVita_BeginOverlayPass" in backend
    assert "void GfxVita_BeginBoundedPass(GfxSurfaceMode mode)" in backend
    assert "GfxVita_BeginBoundedPass(GfxSurfaceMode::Opaque)" in source
    assert "uint32_t remainingIndices = static_cast<uint32_t>(sourceIndexBudget)" in source
    assert '"renderer_overlay", "hud_draw"' in source
    assert "gfx->BindTexture(GfxTextureHandle{0})" in source
    assert "HUD independent from any material mode" in source
    assert "KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT" in source
    assert 'smoke_autostart_shipment' in source
    assert 'smoke_autoplay_input' in source
    assert 'smoke_ads_hold' in source
    assert 'smoke_weapon_swap_mp5' in source
    assert 'smoke_weapon_swap_usp' in source
    assert '"vita_dm_state"' in source
    assert '"smoke_route"' in source
    assert 'scalarMaterialFallback' in source
    assert 'scalarMaterialVertices' in source
    assert 'viewhands_marine_gloves_col' in source
    assert 'weapon_m4_col' in source
    assert "hugeMap ? 8u * 1024u * 1024u" in source
    assert "16'665'600u" in source
    assert 'residentBytes = softwareFallback' in source
    assert 'stage those critical maps' in source
    assert 'void VitaMatrixMultiply' in source
    assert 'void VitaWorldToView' in source
    assert 'source=CG_AddViewWeapon localAngles=0,0,0' in source
    assert 'VitaTransformPoint(rootPlacement, skinnedPosition, worldPosition)' in source
    assert 'VitaWorldToView(cameraWorldPlacement, worldPosition' in source
    assert 'DrawOne(*hands);' in source
    assert 'DrawOne(*model);' in source
    # The retail first-person route has no world-space player triangle between
    # the source viewmodel and HUD. Keep that old diagnostic cue from returning.
    assert 'GfxDrawVertex marker[]' not in source
    assert 'ApplyWeaponTagFrame' not in source
    assert 'kWeaponTagYaw' not in source
    assert 'kWeaponTagPitch' not in source
    assert 'cameraSide * viewScale' not in source
    assert 'cameraVertical * viewScale' not in source
    assert 'cameraDepth * viewScale' not in source
    assert 'const float cameraNormal[3] = {normal[1], normal[2], normal[0]}' not in source
    assert 'VitaSkinPose cameraAttachment' in source
    assert 'viewmodel_camera_tag' in source
    assert 'VitaSkinPose flashAttachment' in source
    assert 'source=tag_flash' in source
    assert 'model.flashTagBone' in source
    # Viewmodel projection must preserve the KisakCOD near-plane behavior on
    # the scalar backend. Edge-clamping a behind-camera vertex creates the
    # stretched diagonal that previously swallowed the Shipment frame.
    assert 'struct VitaModelClipVertex' in source
    assert 'VitaInterpolateModelClipVertex' in source
    assert 'VitaReprojectModelClipVertex' in source
    # Keep the camera-space scratch on ShipmentPreview and reserve it while
    # the model bundle is resident; a late guest-heap allocation here can stall
    # Vita3K with shader/uniform diagnostics enabled.
    assert 'std::vector<std::array<float, 3>> modelCameraPositions' in source
    assert 'void PrepareViewModelScratch' in source
    assert 'preview->modelCameraPositions.reserve(maximumViewModelVertices)' in source
    assert 'preview->modelCameraPositions.resize(source.vertices.size())' in source
    assert 'preview->sourceLightGridSamples.resize' in source
    assert 'preview->sourceSunShadowSamples.resize' in source
    assert 'const auto BuildClippedSurface' in source
    assert 'kViewModelNearClip = 0.1f' in source
    assert 'cameraPositions[index] = {' in source
    assert 'const float projectedX = cameraPosition[0] / safeDepth' in source
    assert 'output.position[0] = projectedX' in source
    assert 'output.position[0] = cameraPosition[0]' in source
    assert 'g_nativeViewModelSpaceDraw = true' in backend
    assert 'uniform float u_viewmodel_space' in backend
    assert 'a_position.z - 2.0 * u_world_near_plane' in backend
    assert 'std::clamp(\n                cameraPosition[0] / safeDepth' not in source
    # Shipment's authored BSP has vertices above the 16-bit draw-index range;
    # preserve their source identity through frustum clipping so a clipped
    # container face cannot alias unrelated grass/ground geometry.
    assert 'point.sourceIndex = sourceIndex' in source
    assert 'point.sourceIndex = static_cast<uint16_t>(index)' not in source
    assert 'ads=%u firing=%u reload=%u' in source
    assert 'viewmodel_placement' in source
    assert 'if (!viewModelDrawn) {' in source
    assert 'const bool scalarMaterialFallback = texture.value == 0' in source
    assert 'bool VitaSourceDefaultWeaponSurfaceVisible' in source
    assert 'WeaponDef::hideTags' in source
    assert 'authoritative selection instead of inferring a model from the weapon' in source
    assert 'weaponDefinition->viewModelName[0] != \'\\0\'' in source
    assert 'return models->Find(weaponDefinition->viewModelName)' in source
    assert 'DB_VitaBuildWeaponHideSurfaceMask' in database_source
    assert 'hideSurfaceMask' in source
    assert 'VitaSourceWeaponSurfaceHidden' in source
    assert 'viewmodel_hide_policy' in source
    assert 'exactHideMask' in source
    assert 'hiddenSourceSurfaces' in source
    assert 'material.find("weapon_reflex")' in source
    assert 'LoadModelTextures(gfx, &shipmentPreview, &shipmentModels' in source
    assert 'VitaScriptServicesTrace("renderer_model_textures", detail)' in source
    assert '"viewmodel_drawn"' in source
    assert 'command.buttons |= BUTTON_ATTACK' in source
    assert 'command.buttons |= BUTTON_RELOAD' in source
    assert 'void DrawHudDigit' in source
    assert 'void DrawHudNumber' in source
    assert 'void DrawCompassPlayerArrow' in source
    assert 'player.angles[1]' in source
    assert 'R_NormalizedTextScale' in source
    assert 'const float normalizedScale = scale * 48.0f' in source
    assert 'gfx_explosion_flash/effect_zfeather_add_nofog' in source
    assert 'SetSurfaceMode(GfxSurfaceMode::Additive)' in source
    assert 'muzzleColor[] = {1.0f, 1.0f, 1.0f, 1.0f}' in source
    assert 'muzzleGlow[]' not in source
    assert 'muzzleCore[]' not in source
    assert 'struct VitaMuzzleAnchor' in source
    assert 'DefaultMuzzleAnchor' in source
    assert '"tag_flash" : "mesh_tip"' in source
    assert 'constexpr float kMuzzleTipBand = 0.025f' in source
    assert 'muzzleAnchor->x = std::clamp(' in source
    assert 'muzzleXSum / static_cast<float>(muzzleVertexCount)' in source
    assert 'DrawShotImpactFx(gfx, *player, lastFireFxTime, lastFireFxPoint,' in source
    assert 'void DrawScreenLine' in source
    assert 'bool ProjectWorldPoint' in source
    assert 'void DrawShotImpactFx' in source
    assert 'event.type == kisak::vita::ShipmentDmEventType::Fire' in source
    assert 'lastFireFxPoint' in source
    cmake = (ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    assert "KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT" in cmake
    assert "KISAK_VITA_ALLOW_SOFTWARE_FALLBACK" in cmake
    assert "KISAK_VITA_MENU_SCREEN" in cmake
    assert "SetDiagnosticScreen" in source
    assert "diagnostic scalar renderer" in cmake
    assert "KISAK_VITA_ALLOW_SOFTWARE_FALLBACK=0" in cmake
    assert "KISAK_VITA_ALLOW_SOFTWARE_FALLBACK=1" in cmake
    assert "accelerated vitaGL/GXM" in backend
    registry = (ROOT / "src" / "database" / "db_registry.cpp").read_text(
        encoding="utf-8"
    )
    assert "XModel parentList stores a backwards offset" in registry
    assert "const uint32_t parentIndex = boneIndex - parentOffset" in registry
    assert 'DB_VitaFindTagBone(model, "tag_camera")' in registry
    assert 'DB_VitaFindTagBone(model, "tag_flash")' in registry
    assert "model.cameraTagBone << 16" in registry
    assert "VitaModelSkinHeaderV5" in registry
    assert "model.boneNames.data()" in registry
    asset_db = (ROOT / "src" / "vita" / "vita_asset_db.cpp").read_text(encoding="utf-8")
    assert "textual bone identities" in asset_db
    assert "hasRecordsHeader(VitaAssetType::Model, 0x58544956, 5)" in asset_db
    assert "hasRecordsHeader(VitaAssetType::Model, 0x58544956, 4)" in asset_db
    assert "hasRecordsHeader(VitaAssetType::Model, 0x58544956, 3)" in asset_db
    print("vita emulator fallback tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
