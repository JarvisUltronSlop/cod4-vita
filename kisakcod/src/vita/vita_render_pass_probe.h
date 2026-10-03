#pragma once
#include <cstdint>
#include "vita_gpu_draw_probe.h"

namespace kisak::vita {
// Phase 0 is ordinary play. Diagnostic phases use unchanged scene commands;
// only phase 2 serializes completion, so its timings include scene-break cost.
constexpr uint32_t VitaRenderProbePhase(uint32_t elapsedMs) {
    return elapsedMs < 60000 ? 1 + elapsedMs / 20000 : 0;
}
// Comparison phases have identical scheduling and no inserted GPU fences.
constexpr uint32_t kVitaPropComparison=0x100;
constexpr uint32_t kVitaPropWorkload=0x200;
constexpr uint32_t kVitaOpaqueComparison=0x400;
constexpr uint32_t kVitaDepthOrderComparison=0x800;
constexpr uint32_t kVitaMaterialProbe=0x1000;
constexpr uint32_t kVitaSpecularComparison=0x2000;
constexpr uint32_t kVitaOpaqueBaseline=0x4000;
constexpr uint32_t kVitaMaterialFamily=0x8000;
constexpr uint32_t kVitaDisplayQueueProbe=0x10000;
constexpr uint32_t kVitaTexturePrefetch=0x20000;
constexpr uint32_t kVitaGpuDrawProbe=0x40000;
constexpr uint32_t kVitaFastColorComparison=0x80000;
constexpr uint32_t kVitaWeaponOpaqueComparison=0x100000;
constexpr uint32_t kVitaHudComparison=0x200000;
constexpr uint32_t kVitaEarlyAlphaComparison=0x400000;
// Same bounded draw sampler, with the measured fast grass/clear baseline.
constexpr uint32_t kVitaFastBaselineDrawProbe=0x800000;
constexpr uint32_t kVitaWeaponPipelineComparison=0x1000000;
constexpr uint32_t kVitaWeaponVertexComparison=0x2000000;
constexpr uint32_t kVitaWeaponSpecularComparison=0x4000000;
constexpr uint32_t kVitaWorldAlphaComparison=0x8000000;
// Refresh draw attribution with the run52 candidate pipeline held constant.
constexpr uint32_t kVitaCurrentDrawProbe=0x10000000;
constexpr uint32_t kVitaPostFilmComparison=0x20000000;
constexpr uint32_t kVitaWorldLifetimeComparison=0x40000000;
constexpr uint32_t kVitaLightmapLayoutComparison=0x80000000u;
// Exact diagnostic tags, not composable feature flags. The one-bit space is full.
constexpr uint32_t kVitaWorldScaling=0x12000000u;
constexpr bool VitaWorldScalingMode(uint32_t phase) {return phase>kVitaWorldScaling && phase<=kVitaWorldScaling+7u;}
constexpr uint32_t VitaWorldScalingPhase(uint32_t elapsedMs) {return elapsedMs<140000?1+elapsedMs/20000:0;}
constexpr uint32_t kVitaWorldPrecisionComparison=0x11000000u;
constexpr bool VitaWorldPrecisionMode(uint32_t phase) {return phase>kVitaWorldPrecisionComparison && phase<=kVitaWorldPrecisionComparison+3u;}
constexpr uint32_t kVitaPassIsolation=0xf0000000u;
constexpr bool VitaPassIsolationMode(uint32_t phase) {return phase>kVitaPassIsolation && phase<=kVitaPassIsolation+9u;}
constexpr uint32_t VitaPassIsolationPhase(uint32_t ms) {return ms<180000?1+ms/20000:0;}
constexpr uint32_t kVitaCompositionComparison=0xe0000000u;
constexpr bool VitaCompositionMode(uint32_t phase) {return phase>kVitaCompositionComparison && phase<=kVitaCompositionComparison+3u;}
constexpr bool VitaCompositionCandidate(uint32_t phase) {return phase==(kVitaCompositionComparison|2u);}
constexpr uint32_t kVitaGraphicsProfile=0xd0000000u;
constexpr bool VitaGraphicsProfileMode(uint32_t phase) {return phase>kVitaGraphicsProfile && phase<=kVitaGraphicsProfile+6u;}
constexpr uint32_t VitaGraphicsProfilePhase(uint32_t ms) {return ms<120000?1+ms/20000:0;}
constexpr uint32_t kVitaWorldComponentsComparison=0xc0000000u;
constexpr bool VitaWorldComponentsMode(uint32_t phase) {return phase>kVitaWorldComponentsComparison && phase<=kVitaWorldComponentsComparison+5u;}
constexpr uint32_t VitaWorldComponentsPhase(uint32_t elapsedMs) {return elapsedMs<100000?1+elapsedMs/20000:0;}
constexpr uint32_t kVitaWorldGlossComparison=0xb0000000u;
constexpr bool VitaWorldGlossMode(uint32_t phase) {return phase>kVitaWorldGlossComparison && phase<=kVitaWorldGlossComparison+3u;}
constexpr uint32_t kVitaWorldBasisComparison=0xa0000000u;
constexpr bool VitaWorldBasisMode(uint32_t phase) {return phase>kVitaWorldBasisComparison && phase<=kVitaWorldBasisComparison+3u;}
constexpr uint32_t kVitaWorldMathComparison=0x90000000u;
constexpr bool VitaWorldMathMode(uint32_t phase) {return phase>kVitaWorldMathComparison && phase<=kVitaWorldMathComparison+3u;}
constexpr bool VitaLightmapLayoutMode(uint32_t phase) {return phase>kVitaLightmapLayoutComparison && phase<=kVitaLightmapLayoutComparison+3u;}
constexpr bool VitaWorldLifetimeMode(uint32_t phase) {return phase>kVitaWorldLifetimeComparison && phase<=kVitaWorldLifetimeComparison+3;}
constexpr bool VitaWorldLifetimeCandidate(uint32_t phase) {return phase==(kVitaWorldLifetimeComparison|2u);}
constexpr bool VitaPostFilmMode(uint32_t phase) {return phase>kVitaPostFilmComparison && phase<=kVitaPostFilmComparison+3;}
constexpr bool VitaPostFilmCandidate(uint32_t phase) {return phase==(kVitaPostFilmComparison|2u);}
constexpr bool VitaCurrentDrawMode(uint32_t phase) {return phase>kVitaCurrentDrawProbe && phase<=kVitaCurrentDrawProbe+3;}
constexpr bool VitaWorldAlphaMode(uint32_t phase) {return phase>kVitaWorldAlphaComparison && phase<=kVitaWorldAlphaComparison+3;}
constexpr bool VitaWorldAlphaCandidate(uint32_t phase) {return phase==(kVitaWorldAlphaComparison|2u);}
constexpr bool VitaWorldAlphaEnabled(uint32_t phase) {return VitaWorldAlphaCandidate(phase) || VitaCurrentDrawMode(phase) || VitaPostFilmMode(phase) || VitaWorldLifetimeMode(phase) || VitaLightmapLayoutMode(phase) || VitaWorldMathMode(phase) || VitaWorldBasisMode(phase) || VitaWorldGlossMode(phase) || VitaWorldComponentsMode(phase) || VitaGraphicsProfileMode(phase) || VitaCompositionMode(phase) || VitaPassIsolationMode(phase) || VitaWorldPrecisionMode(phase) || VitaWorldScalingMode(phase);}
constexpr bool VitaWeaponSpecularMode(uint32_t phase) { return phase>kVitaWeaponSpecularComparison && phase<=kVitaWeaponSpecularComparison+3; }
constexpr bool VitaWeaponSpecularCandidate(uint32_t phase) { return phase==(kVitaWeaponSpecularComparison|2u); }
constexpr bool VitaWeaponSpecularEnabled(uint32_t phase) {return VitaWeaponSpecularCandidate(phase) || VitaWorldAlphaMode(phase) || VitaCurrentDrawMode(phase) || VitaPostFilmMode(phase) || VitaWorldLifetimeMode(phase) || VitaLightmapLayoutMode(phase) || VitaWorldMathMode(phase) || VitaWorldBasisMode(phase) || VitaWorldGlossMode(phase) || VitaWorldComponentsMode(phase) || VitaGraphicsProfileMode(phase) || VitaCompositionMode(phase) || VitaPassIsolationMode(phase) || VitaWorldPrecisionMode(phase) || VitaWorldScalingMode(phase);}
constexpr bool VitaWeaponVertexMode(uint32_t phase) { return phase>kVitaWeaponVertexComparison && phase<=kVitaWeaponVertexComparison+3; }
constexpr bool VitaWeaponVertexCandidate(uint32_t phase) { return phase==(kVitaWeaponVertexComparison|2u); }
constexpr bool VitaWeaponVertexEnabled(uint32_t phase) { return VitaWeaponVertexCandidate(phase) || VitaWeaponSpecularMode(phase) || VitaWorldAlphaMode(phase) || VitaCurrentDrawMode(phase) || VitaPostFilmMode(phase) || VitaWorldLifetimeMode(phase) || VitaLightmapLayoutMode(phase) || VitaWorldMathMode(phase) || VitaWorldBasisMode(phase) || VitaWorldGlossMode(phase) || VitaWorldComponentsMode(phase) || VitaGraphicsProfileMode(phase) || VitaCompositionMode(phase) || VitaPassIsolationMode(phase) || VitaWorldPrecisionMode(phase) || VitaWorldScalingMode(phase); }
constexpr bool VitaWeaponPipelineCandidate(uint32_t phase) { return phase==(kVitaWeaponPipelineComparison|2u); }
constexpr bool VitaWeaponPipelineEnabled(uint32_t phase) { return VitaWeaponPipelineCandidate(phase) || VitaWeaponVertexMode(phase) || VitaWeaponSpecularMode(phase) || VitaWorldAlphaMode(phase) || VitaCurrentDrawMode(phase) || VitaPostFilmMode(phase) || VitaWorldLifetimeMode(phase) || VitaLightmapLayoutMode(phase) || VitaWorldMathMode(phase) || VitaWorldBasisMode(phase) || VitaWorldGlossMode(phase) || VitaWorldComponentsMode(phase) || VitaGraphicsProfileMode(phase) || VitaCompositionMode(phase) || VitaPassIsolationMode(phase) || VitaWorldPrecisionMode(phase) || VitaWorldScalingMode(phase); }
constexpr uint32_t VitaMaterialProbePhase(uint32_t elapsedMs) {
    return elapsedMs<140000?1+elapsedMs/20000:0;
}
constexpr uint32_t VitaPropWorkloadPhase(uint32_t elapsedMs) {
    return elapsedMs<100000?1+elapsedMs/20000:0;
}
constexpr bool VitaRenderProbeValid(uint32_t value) {
    return value<=3 || (value>kVitaPropComparison && value<=kVitaPropComparison+3)
        || (value>kVitaPropWorkload && value<=kVitaPropWorkload+5)
        || (value>kVitaOpaqueComparison && value<=kVitaOpaqueComparison+3)
        || (value>kVitaDepthOrderComparison && value<=kVitaDepthOrderComparison+3)
        || (value>kVitaMaterialProbe && value<=kVitaMaterialProbe+7)
        || (value>kVitaSpecularComparison && value<=kVitaSpecularComparison+3)
        || (value>kVitaOpaqueBaseline && value<=kVitaOpaqueBaseline+3)
        || (value>kVitaMaterialFamily && value<=kVitaMaterialFamily+3)
        || (value>kVitaDisplayQueueProbe && value<=kVitaDisplayQueueProbe+3)
        || (value>kVitaTexturePrefetch && value<=kVitaTexturePrefetch+3)
        || (value>kVitaGpuDrawProbe && value<=kVitaGpuDrawProbe+3)
        || (value>kVitaFastColorComparison && value<=kVitaFastColorComparison+3)
        || (value>kVitaWeaponOpaqueComparison && value<=kVitaWeaponOpaqueComparison+3)
        || (value>kVitaHudComparison && value<=kVitaHudComparison+3)
        || (value>kVitaEarlyAlphaComparison && value<=kVitaEarlyAlphaComparison+3)
        || (value>kVitaFastBaselineDrawProbe && value<=kVitaFastBaselineDrawProbe+3)
        || (value>kVitaWeaponPipelineComparison && value<=kVitaWeaponPipelineComparison+3) || VitaWeaponVertexMode(value) || VitaWeaponSpecularMode(value) || VitaWorldAlphaMode(value) || VitaCurrentDrawMode(value) || VitaPostFilmMode(value) || VitaWorldLifetimeMode(value) || VitaLightmapLayoutMode(value) || VitaWorldMathMode(value) || VitaWorldBasisMode(value) || VitaWorldGlossMode(value) || VitaWorldComponentsMode(value) || VitaGraphicsProfileMode(value) || VitaCompositionMode(value) || VitaPassIsolationMode(value) || VitaWorldPrecisionMode(value) || VitaWorldScalingMode(value);
}
enum class VitaRenderPass : uint32_t { Setup, World, PropsEffects, Composition, Weapon, Post, Hud, Count };
struct VitaRenderPassProbe {
    uint32_t requestedPhase=0,phase=0, seen=0, draws=0, triangles=0;
    bool comparison=false,workload=false,opaque=false,depthOrder=false,material=false,specular=false,baseline=false,family=false,displayQueue=false,prefetch=false,drawProbe=false,fastColor=false,weaponOpaque=false,hud=false,earlyAlpha=false,fastDrawProbe=false,weaponPipeline=false,weaponVertex=false,weaponSpecular=false,worldAlpha=false,currentDrawProbe=false,postFilm=false,worldLifetime=false,lightmapLayout=false,worldMath=false,worldBasis=false,worldGloss=false,worldComponents=false,graphicsProfile=false,composition=false,passIsolation=false,worldPrecision=false,worldScaling=false;
    uint64_t beginUs=0, lastUs=0, waitUs=0;
    void Begin(uint32_t requested, uint64_t now) {
        *this={};if(VitaRenderProbeValid(requested)){requestedPhase=requested;worldPrecision=VitaWorldPrecisionMode(requested);worldScaling=VitaWorldScalingMode(requested);passIsolation=VitaPassIsolationMode(requested);composition=VitaCompositionMode(requested);phase=requested&0xff;comparison=(requested&kVitaPropComparison)!=0;workload=(requested&kVitaPropWorkload)!=0;opaque=(requested&kVitaOpaqueComparison)!=0;depthOrder=(requested&kVitaDepthOrderComparison)!=0;material=(requested&kVitaMaterialProbe)!=0;specular=(requested&kVitaSpecularComparison)!=0;baseline=(requested&kVitaOpaqueBaseline)!=0;family=(requested&kVitaMaterialFamily)!=0;displayQueue=(requested&kVitaDisplayQueueProbe)!=0;prefetch=(requested&kVitaTexturePrefetch)!=0;currentDrawProbe=VitaCurrentDrawMode(requested);fastDrawProbe=(requested&kVitaFastBaselineDrawProbe)!=0;drawProbe=(requested&kVitaGpuDrawProbe)!=0 || fastDrawProbe || currentDrawProbe;fastColor=(requested&kVitaFastColorComparison)!=0;weaponOpaque=(requested&kVitaWeaponOpaqueComparison)!=0;hud=(requested&kVitaHudComparison)!=0;earlyAlpha=(requested&kVitaEarlyAlphaComparison)!=0;graphicsProfile=VitaGraphicsProfileMode(requested);worldComponents=VitaWorldComponentsMode(requested);worldGloss=VitaWorldGlossMode(requested);worldBasis=VitaWorldBasisMode(requested);worldMath=VitaWorldMathMode(requested);lightmapLayout=VitaLightmapLayoutMode(requested);worldLifetime=VitaWorldLifetimeMode(requested);postFilm=VitaPostFilmMode(requested);worldAlpha=VitaWorldAlphaMode(requested) || currentDrawProbe || postFilm || worldLifetime || lightmapLayout || worldMath || worldBasis || worldGloss || worldComponents || graphicsProfile || composition || passIsolation || worldPrecision || worldScaling;weaponSpecular=VitaWeaponSpecularMode(requested) || worldAlpha;weaponVertex=VitaWeaponVertexMode(requested) || weaponSpecular;weaponPipeline=(requested&kVitaWeaponPipelineComparison)!=0 || weaponVertex;}
        beginUs=lastUs=now;
    }
    bool WorldAlphaActive() const {return VitaWorldAlphaEnabled(requestedPhase);}
    bool Observe(VitaRenderPass pass) {
        const auto index=static_cast<uint32_t>(pass);
        if(!phase || index>=static_cast<uint32_t>(VitaRenderPass::Count) || (seen&(1u<<index)))return false;
        seen|=1u<<index;return true;
    }
    bool Fenced() const {return phase==2 && !comparison && !workload && !opaque && !depthOrder && !material && !specular && !baseline && !family && !displayQueue && !prefetch && !drawProbe && !fastColor && !weaponOpaque && !hud && !earlyAlpha && !weaponPipeline;}
    bool ClipProps() const {return workload && phase==2;}
    bool SuppressProps() const {return workload && phase==4;}
    bool Complete() const {return seen==127;}
};
}
