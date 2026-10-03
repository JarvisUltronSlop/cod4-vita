#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_index.h>
#include <vita/vita_script_services.h>
#include <vita/vita_source_ui_backend.h>
#include <vita/vita_source_render.h>
#include <vita/vita_material_bundle.h>
#include <vita/vita_image_bundle.h>
#include <universal/q_shared.h>
#include <gfx/gfx_backend.h>
#include <gfx_d3d/rb_backend.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_material.h>
#include <psp2/kernel/clib.h>
#include <cmath>
#include <cstring>

namespace kisak::vita {
namespace {
constexpr uint32_t kMaxMaterials = 1024, kBatchQuads = 64, kMaxFrameQuads = 16384;
struct Binding { const VitaMaterialRecord* material; GfxTextureHandle texture; bool owned; };
Binding bindings[kMaxMaterials]{};
uint32_t bindingCount = 0;
VitaCpuPointerIndex<2048> resolvedMaterials;
const GfxBackendApi* drawingGfx = nullptr;
const Binding* batchBinding = nullptr;
const Material* previousMaterial = nullptr;
const Binding* previousBinding = nullptr;
GfxDrawVertex vertices[kBatchQuads * 4]{};
uint16_t indices[kBatchQuads * 6]{};
uint32_t batchQuads = 0;
VitaSourceUiBackendReport report;
uint64_t omittedTotal = 0;
uint32_t loggedNames[64]{}, loggedNameCount = 0, frameCount = 0;

void Omission(const char* name, const char* reason) {
    ++omittedTotal;
    uint32_t hash = 2166136261u;
    for (const char* c = name; *c; ++c) hash = (hash ^ static_cast<uint8_t>(*c)) * 16777619u;
    for (uint32_t i = 0; i < loggedNameCount; ++i) if (loggedNames[i] == hash) return;
    if (loggedNameCount == 64) return;
    loggedNames[loggedNameCount++] = hash;
    VitaScriptServicesTrace("source_ui_omission", name);
    sceClibPrintf("[kisak-vita] source UI omitted material=%s reason=%s\n", name, reason);
}
const Binding* Resolve(const Material* material) {
    if (material == previousMaterial) return previousBinding;
    previousMaterial = material;
    previousBinding = nullptr;
    auto* cached = material ? resolvedMaterials.Find(reinterpret_cast<uintptr_t>(material), 0) : nullptr;
    if (cached && cached->id) {
        if (cached->id <= bindingCount) previousBinding = &bindings[cached->id - 1];
        return previousBinding;
    }
    if (material && material->info.name) {
        for (uint32_t i = 0; i < bindingCount; ++i)
            if (bindings[i].material->name == material->info.name) {
                previousBinding = &bindings[i];
                break;
            }
    }
    if (cached) *cached = {reinterpret_cast<uintptr_t>(material), 0,
        uint16_t(previousBinding ? previousBinding - bindings + 1 : UINT16_MAX)};
    return previousBinding;
}
void Flush() {
    if (!batchQuads || !drawingGfx || !batchBinding) return;
    const auto& material = *batchBinding->material;
    GfxMaterialPassState pass{};
    pass.colorTexture = batchBinding->texture;
    pass.colorSamplerState = material.colorSamplerState;
    pass.techniqueFlags0 = GFX_MATERIAL_PASS_SOURCE_UI | GFX_MATERIAL_PASS_SOURCE_UNLIT;
    pass.surfaceMode = GfxSurfaceMode::Overlay;
    pass.sourceStateBits0 = material.unlitStateBits0;
    pass.sourceStateBits1 = material.unlitStateBits1;
    pass.sourceStateValid = material.unlitStateValid;
    drawingGfx->SetSurfaceMode(GfxSurfaceMode::Overlay);
    drawingGfx->SetMaterialPass(&pass);
    drawingGfx->BindTexture(batchBinding->texture);
    drawingGfx->DrawIndexed(vertices, batchQuads * 4, indices, batchQuads * 6);
    ++report.batches;
    batchQuads = 0;
}
void Quad(const Material* material, const float (*corners)[2],
          const float (*uv)[2], const float* color) {
    if (!drawingGfx) return;
    const auto* binding = Resolve(material);
    if (!binding) {
        ++report.missingMaterials;
        Omission(material && material->info.name ? material->info.name : "<null>",
                 "unlit state or resident source image unavailable");
        return;
    }
    if (report.quads == kMaxFrameQuads) {
        ++report.unsupportedCommands;
        Omission("<quad capacity>", "bounded source UI submission exhausted");
        return;
    }
    if (binding != batchBinding || batchQuads == kBatchQuads) Flush();
    batchBinding = binding;
    const uint16_t base = static_cast<uint16_t>(batchQuads * 4);
    for (uint32_t i = 0; i < 4; ++i) {
        auto& v = vertices[base + i];
        v.position[0] = corners[i][0] / 480.0f - 1.0f;
        v.position[1] = 1.0f - corners[i][1] / 272.0f;
        v.position[2] = 0.0f;
        std::memcpy(v.texCoord, uv[i], sizeof(v.texCoord));
        std::memcpy(v.color, color, sizeof(v.color));
    }
    // Preserve the source text tessellation order.
    const uint16_t local[6]{3, 0, 2, 2, 0, 1};
    for (uint32_t i = 0; i < 6; ++i) indices[batchQuads * 6 + i] = base + local[i];
    ++batchQuads;
    ++report.quads;
}
}

void VitaSourceUiBackend_Reset(const GfxBackendApi* gfx) {
    // Called only after source drawing has stopped on the same main thread.
    if (gfx && gfx->DestroyTexture)
        for (uint32_t i = 0; i < bindingCount; ++i)
            if (bindings[i].owned) gfx->DestroyTexture(bindings[i].texture);
    bindingCount = batchQuads = frameCount = loggedNameCount = 0;
    omittedTotal = 0;
    drawingGfx = nullptr;
    batchBinding = previousBinding = nullptr;
    previousMaterial = nullptr;
    resolvedMaterials.Reset();
}
bool VitaSourceUiBackend_Ready() { return bindingCount != 0; }
bool VitaSourceUiBackend_Stage(const GfxBackendApi* gfx,
    const VitaMaterialBundle& materials, const VitaImageBundle& images,
    const VitaImageBundle* commonImages, const char* mapName, GfxTextureHandle mapCompassTexture,
    void (*progress)(uint32_t, uint32_t, void*), void* progressContext) {
    VitaSourceUiBackend_Reset(gfx);
    if (!gfx || !gfx->CreateTexture2D || !gfx->DestroyTexture) return false;
    uint32_t unavailable = 0;
    const std::string mapCompass = std::string("compass_map_mp_") + (mapName ? mapName : "");
    for (uint32_t i = 0; i < materials.Count(); ++i) {
        if (progress && (i % 16u) == 0u) progress(i, materials.Count(), progressContext);
        const auto* material = materials.Record(i);
        if (!material || !material->unlitStateValid || material->colorSemantic != 0
            || material->colorMap.empty()) continue;
        if (material->colorMap == mapCompass && mapCompassTexture.value) {
            if (bindingCount == kMaxMaterials) { VitaSourceUiBackend_Reset(gfx); return false; }
            // The map owns this already-uploaded image. No second allocation
            // or retained compressed payload is needed for the source HUD.
            bindings[bindingCount++] = {material, mapCompassTexture, false};
            continue;
        }
        // The map VITI also contains unrelated retail UI discovered by the
        // desktop database. Only the explicit common UI closure and this
        // map's compass belong to the active source HUD resource lifetime.
        const auto* image = commonImages ? commonImages->Find(material->colorMap.c_str()) : nullptr;
        if (material->colorMap == mapCompass)
            image = images.Find(material->colorMap.c_str());
        // The source database contains UI for every retail mode. Missing
        // unrelated assets are counted here; actual draws name each omission.
        if (!image || image->bytes.empty()) { ++unavailable; continue; }
        if (bindingCount == kMaxMaterials) { VitaSourceUiBackend_Reset(gfx); return false; }
        const auto texture = gfx->CreateTexture2D(image->bytes.data(), image->bytes.size(),
            image->width, image->height, image->levelCount, image->format, image->flags);
        if (!texture.value) {
            sceClibPrintf("[kisak-vita] source UI staging failed material=%s image=%s bytes=%u\n",
                material->name.c_str(), image->name.c_str(),
                static_cast<unsigned>(image->bytes.size()));
            VitaSourceUiBackend_Reset(gfx);
            return false;
        }
        bindings[bindingCount++] = {material, texture, true};
    }
    sceClibPrintf("[kisak-vita] source UI staged materials=%u unavailableCatalog=%u\n",
                  bindingCount, unavailable);
    if (progress) progress(materials.Count(), materials.Count(), progressContext);
    return bindingCount != 0;
}

VitaSourceUiBackendReport VitaSourceUiBackend_Draw(const GfxBackendApi* gfx,
    const VitaRenderPacket& packet) {
#if KISAK_VITA_CPU_RUNTIME
    VITA_CPU_SCOPE("source-ui-prepare");
#endif
    report = {};
    if (!gfx || !gfx->SetMaterialPass || !gfx->SetSurfaceMode || !gfx->DrawIndexed
        || !gfx->BindTexture) return report;
    drawingGfx = gfx;
    batchBinding = previousBinding = nullptr;
    previousMaterial = nullptr;
    batchQuads = 0;
    for (uint32_t i = 0; i < packet.uiCommandCount; ++i) {
        const auto* ui = VitaSourceRender_Ui(packet.frameGeneration, i);
        if (!ui) { ++report.unsupportedCommands; continue; }
        ++report.commands;
        if (ui->kind == VitaSourceUiKind::Quad) {
            Quad(static_cast<const Material*>(VitaSourceRender_Resource(packet.frameGeneration, ui->resource)),
                 ui->corners, ui->texcoords, ui->color);
        } else if (ui->kind == VitaSourceUiKind::Text) {
            const auto* cmd = VitaSourceRender_Text(packet.frameGeneration, i);
            const float radians = cmd->rotation * 0.01745329251994329577f;
            DrawText2D(cmd->text, cmd->x, cmd->y, cmd->font, cmd->xScale, cmd->yScale,
                std::sin(radians), std::cos(radians), cmd->color, cmd->maxChars,
                static_cast<short>(cmd->renderFlags), cmd->cursorPos, cmd->cursorLetter,
                cmd->padding, cmd->glowForceColor, cmd->fxBirthTime, cmd->fxLetterTime,
                cmd->fxDecayStartTime, cmd->fxDecayDuration, cmd->fxMaterial, cmd->fxMaterialGlow);
        } else {
            ++report.unsupportedCommands;
            Omission("<saved screen>", "source shellshock framebuffer composition pending");
        }
    }
    Flush();
    gfx->SetMaterialPass(nullptr);
    gfx->SetSurfaceMode(GfxSurfaceMode::Overlay);
    gfx->BindTexture(GfxTextureHandle{0});
    drawingGfx = nullptr;
    if (++frameCount == 1 || frameCount % 120 == 0)
        sceClibPrintf("[kisak-vita] source UI frame=%u commands=%u quads=%u missing=%u unsupported=%u omittedTotal=%llu\n",
            frameCount, report.commands, report.quads, report.missingMaterials,
            report.unsupportedCommands, static_cast<unsigned long long>(omittedTotal));
    VitaCpuProfile_Counter("source-ui-packet", report.commands, packet.rejectedCommands);
    VitaCpuProfile_Counter("source-ui-quads", report.quads, report.batches);
    VitaCpuProfile_Counter("source-ui-errors", report.missingMaterials, report.unsupportedCommands);
    return report;
}
}

// Platform boundary for the original DrawText2D layout in rb_text.cpp.
// Glyphs, wrapping, cursor, color escapes and pulse/glow remain source-owned.
void RB_DrawStretchPicRotate(const Material* material, float x, float y, float w,
    float h, float s0, float t0, float s1, float t1, float sinAngle, float cosAngle,
    uint32_t packed, GfxPrimStatsTarget) {
    const float dx = w * cosAngle, dy = w * sinAngle;
    const float hx = -h * sinAngle, hy = h * cosAngle;
    const float corners[4][2]{{x,y},{x+dx,y+dy},{x+dx+hx,y+dy+hy},{x+hx,y+hy}};
    const float uv[4][2]{{s0,t0},{s1,t0},{s1,t1},{s0,t1}};
    const float color[4]{((packed >> 16) & 255) / 255.0f, ((packed >> 8) & 255) / 255.0f,
                         (packed & 255) / 255.0f, (packed >> 24) / 255.0f};
    kisak::vita::Quad(material, corners, uv, color);
}
