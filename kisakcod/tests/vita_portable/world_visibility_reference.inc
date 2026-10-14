// Frozen from e9ef7f16220a68cd0ef9a36fdbafee0afb97feb9.
// Compile the old draw path against the same recording backend as the candidate.
// Keep this independent of the optimized visibility/order helpers.

bool ReferenceBoundsVisible(const NativeViewFrustum& frustum,
                         const ShipmentSourceBounds& bounds) {
    if (!bounds.valid)
        return true;
    const float center[3] = {
        (bounds.minimum[0] + bounds.maximum[0]) * 0.5f,
        (bounds.minimum[1] + bounds.maximum[1]) * 0.5f,
        (bounds.minimum[2] + bounds.maximum[2]) * 0.5f,
    };
    const float extent[3] = {
        (bounds.maximum[0] - bounds.minimum[0]) * 0.5f,
        (bounds.maximum[1] - bounds.minimum[1]) * 0.5f,
        (bounds.maximum[2] - bounds.minimum[2]) * 0.5f,
    };
    const auto Dot = [](const float left[3], const float right[3]) {
        return left[0] * right[0] + left[1] * right[1]
            + left[2] * right[2];
    };
    const auto Outside = [&](float nx, float ny, float nz, float d) {
        const float distance = nx * center[0] + ny * center[1]
            + nz * center[2] + d;
        const float radius = std::fabs(nx) * extent[0]
            + std::fabs(ny) * extent[1] + std::fabs(nz) * extent[2];
        return distance + radius < 0.0f;
    };
    if (Outside(frustum.forward[0], frustum.forward[1], frustum.forward[2],
                -frustum.eyeForward - frustum.nearDistance)
        || Outside(-frustum.forward[0], -frustum.forward[1],
                   -frustum.forward[2],
                   frustum.eyeForward + frustum.farDistance))
        return false;
    const float left[3] = {
        frustum.forward[0]
            + frustum.right[0] * frustum.horizontalScale,
        frustum.forward[1]
            + frustum.right[1] * frustum.horizontalScale,
        frustum.forward[2]
            + frustum.right[2] * frustum.horizontalScale,
    };
    const float right[3] = {
        frustum.forward[0]
            - frustum.right[0] * frustum.horizontalScale,
        frustum.forward[1]
            - frustum.right[1] * frustum.horizontalScale,
        frustum.forward[2]
            - frustum.right[2] * frustum.horizontalScale,
    };
    const float bottom[3] = {
        frustum.forward[0] + frustum.up[0] * frustum.verticalScale,
        frustum.forward[1] + frustum.up[1] * frustum.verticalScale,
        frustum.forward[2] + frustum.up[2] * frustum.verticalScale,
    };
    const float top[3] = {
        frustum.forward[0] - frustum.up[0] * frustum.verticalScale,
        frustum.forward[1] - frustum.up[1] * frustum.verticalScale,
        frustum.forward[2] - frustum.up[2] * frustum.verticalScale,
    };
    const auto PlaneD = [&](const float normal[3]) {
        return -Dot(normal, frustum.eye);
    };
    return !Outside(left[0], left[1], left[2], PlaneD(left))
        && !Outside(right[0], right[1], right[2], PlaneD(right))
        && !Outside(bottom[0], bottom[1], bottom[2], PlaneD(bottom))
        && !Outside(top[0], top[1], top[2], PlaneD(top));
}

bool ReferenceDrawNativeWorld(const GfxBackendApi* gfx, ShipmentPreview* preview,
                     const kisak::vita::ShipmentDmPlayerState& player,
                     float fovDegrees,
                     kisak::vita::VitaVisibilityReport* report = nullptr, uint32_t depthOrderPhase=0) {
#if KISAK_VITA_CPU_RUNTIME
    VITA_CPU_SCOPE("source-world-prepare");
#endif
    if (report)
        *report = {};
#if KISAK_VITA_SOURCE_RUNTIME
    if (!kisak::vita::VitaSourceRender_Packet().sourceViewValid) return false;
#endif
    if (!gfx || !preview || !preview->nativeArchive.ready
        || preview->nativeArchive.vertexBuffer.value == 0
        || preview->nativeArchive.indexBuffer.value == 0
        || !gfx->SetWorldViewProjection || !gfx->DrawStaticIndexed)
        return false;
    const NativeViewFrustum frustum = BuildNativeViewFrustum(player, fovDegrees);
    const float kNear = frustum.nearDistance;
    const float kFar = frustum.farDistance;
    const VitaProjection projection{frustum.horizontalScale, frustum.verticalScale};
    const float* forward = frustum.forward;
    const float* right = frustum.right;
    const float* up = frustum.up;
    const float* eye = frustum.eye;
    const auto Dot = [](const float a[3], const float b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const float eyeForward = Dot(forward, eye);
    const float eyeRight = Dot(right, eye);
    const float eyeUp = Dot(up, eye);
#if KISAK_VITA_SOURCE_RUNTIME
    gfx->SetWorldViewProjection(
        kisak::vita::VitaSourceRender_Packet().viewProjection, kNear);
#else
    float matrix[16]{};
    matrix[0] = right[0] * projection.horizontalScale;
    matrix[4] = right[1] * projection.horizontalScale;
    matrix[8] = right[2] * projection.horizontalScale;
    matrix[12] = -eyeRight * projection.horizontalScale;
    matrix[1] = up[0] * projection.verticalScale;
    matrix[5] = up[1] * projection.verticalScale;
    matrix[9] = up[2] * projection.verticalScale;
    matrix[13] = -eyeUp * projection.verticalScale;
    // OpenGL window depth maps NDC [-1,+1] to [0,1]. z-2*near therefore
    // produces the exact 1-near/z encoding used by the scalar oracle.
    matrix[2] = forward[0];
    matrix[6] = forward[1];
    matrix[10] = forward[2];
    matrix[14] = -eyeForward - 2.0f * kNear;
    matrix[3] = forward[0];
    matrix[7] = forward[1];
    matrix[11] = forward[2];
    matrix[15] = -eyeForward;
    gfx->SetWorldViewProjection(matrix, kNear);
#endif

    const auto OutsidePlane = [](const ShipmentSourceBounds& bounds,
                                 float nx, float ny, float nz, float d) {
        if (!bounds.valid)
            return false;
        const float center[3] = {
            (bounds.minimum[0] + bounds.maximum[0]) * 0.5f,
            (bounds.minimum[1] + bounds.maximum[1]) * 0.5f,
            (bounds.minimum[2] + bounds.maximum[2]) * 0.5f,
        };
        const float extent[3] = {
            (bounds.maximum[0] - bounds.minimum[0]) * 0.5f,
            (bounds.maximum[1] - bounds.minimum[1]) * 0.5f,
            (bounds.maximum[2] - bounds.minimum[2]) * 0.5f,
        };
        const float distance = nx * center[0] + ny * center[1]
            + nz * center[2] + d;
        const float radius = std::fabs(nx) * extent[0]
            + std::fabs(ny) * extent[1] + std::fabs(nz) * extent[2];
        return distance + radius < 0.0f;
    };
    const auto SurfaceVisible = [&](const ShipmentSourceBounds& bounds) {
        const auto Rejected = [&](float nx, float ny, float nz, float d) {
            return OutsidePlane(bounds, nx, ny, nz, d);
        };
        const float hx = projection.horizontalScale;
        const float vy = projection.verticalScale;
        if (Rejected(forward[0], forward[1], forward[2],
                     -eyeForward - kNear)
            || Rejected(-forward[0], -forward[1], -forward[2],
                        eyeForward + kFar))
            return false;
        const float left[3] = {forward[0] + right[0] * hx,
                               forward[1] + right[1] * hx,
                               forward[2] + right[2] * hx};
        const float rightPlane[3] = {forward[0] - right[0] * hx,
                                     forward[1] - right[1] * hx,
                                     forward[2] - right[2] * hx};
        const float bottom[3] = {forward[0] + up[0] * vy,
                                 forward[1] + up[1] * vy,
                                 forward[2] + up[2] * vy};
        const float top[3] = {forward[0] - up[0] * vy,
                              forward[1] - up[1] * vy,
                              forward[2] - up[2] * vy};
        return !Rejected(left[0], left[1], left[2], -Dot(left, eye))
            && !Rejected(rightPlane[0], rightPlane[1], rightPlane[2],
                         -Dot(rightPlane, eye))
            && !Rejected(bottom[0], bottom[1], bottom[2], -Dot(bottom, eye))
            && !Rejected(top[0], top[1], top[2], -Dot(top, eye));
    };

    const uint64_t visibilityStart = sceKernelGetProcessTimeWide();
    preview->visibleWorldSurfaces.clear();
    for (uint32_t index = 0; index < preview->surfaces.size(); ++index) {
        const ShipmentSurface& surface = preview->surfaces[index];
        if (surface.cameraRegion == 3u || surface.mode == GfxSurfaceMode::Sky
            || surface.vertexCount == 0 || surface.triangleCount == 0)
            continue;
        if (index < preview->sourceSurfaceBounds.size()
            && !SurfaceVisible(preview->sourceSurfaceBounds[index]))
            continue;
        preview->visibleWorldSurfaces.push_back(index);
    }
    std::sort(preview->visibleWorldSurfaces.begin(),
              preview->visibleWorldSurfaces.end(),
              [&](uint32_t leftIndex, uint32_t rightIndex) {
        const ShipmentSurface& left = preview->surfaces[leftIndex];
        const ShipmentSurface& rightSurface = preview->surfaces[rightIndex];
        const uint64_t leftPhase = static_cast<uint8_t>(left.mode);
        const uint64_t rightPhase = static_cast<uint8_t>(rightSurface.mode);
        if (leftPhase != rightPhase)
            return leftPhase < rightPhase;
        if (left.sourceTechniqueFlags0 != rightSurface.sourceTechniqueFlags0)
            return left.sourceTechniqueFlags0
                < rightSurface.sourceTechniqueFlags0;
        if (left.sourceStateBits0 != rightSurface.sourceStateBits0)
            return left.sourceStateBits0 < rightSurface.sourceStateBits0;
        if (left.sourceStateBits1 != rightSurface.sourceStateBits1)
            return left.sourceStateBits1 < rightSurface.sourceStateBits1;
        if (left.texture.value != rightSurface.texture.value)
            return left.texture.value < rightSurface.texture.value;
        if (left.lightmapTexture.value != rightSurface.lightmapTexture.value)
            return left.lightmapTexture.value
                < rightSurface.lightmapTexture.value;
        if (left.secondaryLightmapTexture.value
            != rightSurface.secondaryLightmapTexture.value)
            return left.secondaryLightmapTexture.value
                < rightSurface.secondaryLightmapTexture.value;
        return leftIndex < rightIndex;
    });

#if KISAK_VITA_CPU_RUNTIME && KISAK_VITA_SOURCE_RUNTIME
    if(depthOrderPhase) {
        const auto sortStart=kisak::vita::VitaCpuNow();
        struct DepthEvidence {uint32_t swaps=0,checks=0,eligible=0,invalid=0,before=0,after=0,phase=0;uint64_t usec=0;};
        DepthEvidence evidence{};evidence.phase=depthOrderPhase;
        evidence.before=kisak::vita::VitaDepthMembership(preview->visibleWorldSurfaces.data(),preview->visibleWorldSurfaces.size());
        if(preview->worldDepthRanges.size()<preview->surfaces.size())evidence.invalid=1;
        else {
            const auto* matrix=kisak::vita::VitaSourceRender_Packet().viewProjection;
            for(uint32_t index:preview->visibleWorldSurfaces) {
                auto& range=preview->worldDepthRanges[index];range={};
                const auto& surface=preview->surfaces[index];
                if(index>=preview->sourceSurfaceBounds.size())continue;
                const auto& bounds=preview->sourceSurfaceBounds[index];
                if(bounds.valid && !(surface.sourceTechniqueFlags0 & (GFX_MATERIAL_PASS_SOURCE_UI | GFX_MATERIAL_PASS_SOURCE_UNLIT))
                    && kisak::vita::VitaDepthOrderState(surface.sourceStateValid,
                    surface.mode==GfxSurfaceMode::Opaque,surface.sourceStateBits0,surface.sourceStateBits1))
                    range=kisak::vita::VitaProjectDepthRange(matrix,bounds.minimum,bounds.maximum);
                evidence.eligible+=range.valid;
            }
            if(depthOrderPhase==2) {
                const auto stats=kisak::vita::VitaOrderWorldDepth(preview->visibleWorldSurfaces.data(),preview->visibleWorldSurfaces.size(),preview->worldDepthRanges.data());
                evidence.swaps=stats.swaps;evidence.checks=stats.comparisons;
            }
        }
        evidence.after=kisak::vita::VitaDepthMembership(preview->visibleWorldSurfaces.data(),preview->visibleWorldSurfaces.size());
        evidence.usec=kisak::vita::VitaCpuNow()-sortStart;
        kisak::vita::VitaCpuRender_Copy(evidence,+[](const DepthEvidence& e) {
            kisak::vita::VitaCpuProfile_Counter("render-probe-depth-order",e.swaps,e.checks);
            kisak::vita::VitaCpuProfile_Counter("render-probe-depth-eligible",e.eligible,e.invalid);
            kisak::vita::VitaCpuProfile_Counter("render-probe-depth-membership",e.before,e.after);
            kisak::vita::VitaCpuProfile_Counter("render-probe-depth-sort-us",e.usec,e.phase);
        });
    }
#endif

    const uint64_t drawStart = sceKernelGetProcessTimeWide();
    if (report) {
        report->sourceSurfaces = preview->nativeArchive.surfaceCount;
        report->visibleSurfaces = static_cast<uint32_t>(
            preview->visibleWorldSurfaces.size());
        report->visibilityUsec = static_cast<uint32_t>(std::min<uint64_t>(
            drawStart - visibilityStart, UINT32_MAX));
    }

    const ShipmentSurface* previous = nullptr;
    uint32_t submitted = 0;
    for (uint32_t surfaceIndex : preview->visibleWorldSurfaces) {
        const ShipmentSurface& surface = preview->surfaces[surfaceIndex];
#if KISAK_VITA_BENCHMARK
        const unsigned debrisCategory=kisak::vita::VitaBenchmarkDebrisMaterial(surface.materialName)?0u:1u;
        if (!debrisBenchmark.Admit(debrisCategory,surface.triangleCount*3u)) continue;
#endif
        if (!previous || !SameNativeWorldState(*previous, surface)) {
            if (gfx->SetSurfaceMode)
                gfx->SetSurfaceMode(surface.mode);
            if (gfx->SetAlphaTestFunc)
                gfx->SetAlphaTestFunc(surface.alphaTestFunc);
            if (gfx->SetCullMode)
                gfx->SetCullMode(surface.cullMode);
            if (gfx->SetMaterialPass) {
                GfxMaterialPassState pass{};
                pass.colorTexture = surface.texture;
                pass.normalTexture = surface.normalTexture;
                pass.specularTexture = surface.specularTexture;
                pass.reflectionProbeTexture = surface.reflectionProbeTexture;
                pass.lightmapPrimaryTexture = surface.lightmapTexture;
                pass.lightmapSecondaryTexture = surface.secondaryLightmapTexture;
                pass.colorSamplerState = surface.colorSamplerState;
                pass.normalSamplerState = surface.normalSamplerState;
                pass.specularSamplerState = surface.specularSamplerState;
                pass.lightmapSamplerFlags = surface.lightmapSamplerFlags;
                pass.techniqueFlags0 = surface.sourceTechniqueFlags0;
                pass.surfaceMode = surface.mode;
                pass.alphaTestFunc = surface.alphaTestFunc;
                pass.cullMode = surface.cullMode;
                pass.sourceSurfaceIndex = surfaceIndex;
                pass.sourceStateBits0 = surface.sourceStateBits0;
                pass.sourceStateBits1 = surface.sourceStateBits1;
                pass.sourceStateValid = surface.sourceStateValid;
                gfx->SetMaterialPass(&pass);
            }
            if (gfx->BindTexture)
                gfx->BindTexture(surface.texture);
            if (gfx->SetSamplerState)
                gfx->SetSamplerState(0u, surface.colorSamplerState);
            previous = &surface;
        }
        // Material binding is cached across surfaces; draw identity is not.
        GfxVita_SetDrawSurfaceIndex(surfaceIndex, surface.constantWorldBasis);
        if(gfx->SetDrawLightBounds) {
            GfxDrawLightBounds bounds{};
            if(surfaceIndex<preview->sourceSurfaceBounds.size()) {
                const auto& b=preview->sourceSurfaceBounds[surfaceIndex];
                bounds={{b.minimum[0],b.minimum[1],b.minimum[2]},
                    {b.maximum[0],b.maximum[1],b.maximum[2]},b.valid};
            }
            gfx->SetDrawLightBounds(&bounds);
        }
        const bool surfaceSubmitted=gfx->DrawStaticIndexed(
                preview->nativeArchive.vertexBuffer, surface.firstVertex,
                surface.vertexCount, preview->nativeArchive.indexBuffer,
                surface.baseIndex, surface.triangleCount * 3u);
#if KISAK_VITA_BENCHMARK
        debrisBenchmark.Submitted(debrisCategory,surface.triangleCount*3u,1,surfaceSubmitted);
#endif
        if (surfaceSubmitted) ++submitted;
    }
    if (gfx->SetSurfaceMode)
        gfx->SetSurfaceMode(GfxSurfaceMode::AlphaBlend);
    if (gfx->SetCullMode)
        gfx->SetCullMode(GfxCullMode::None);
    if (gfx->BindTexture)
        gfx->BindTexture(GfxTextureHandle{0});
    if (report) {
        report->submittedSurfaces = submitted;
        report->drawSubmitUsec = static_cast<uint32_t>(std::min<uint64_t>(
            sceKernelGetProcessTimeWide() - drawStart, UINT32_MAX));
    }
    return submitted != 0;
}
