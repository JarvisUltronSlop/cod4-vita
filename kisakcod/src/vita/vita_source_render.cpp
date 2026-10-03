#include <vita/vita_source_render.h>
#include <vita/vita_source_platform.h>
#include <vita/vita_cpu_index.h>
#include <vita/vita_cpu_profile.h>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <bgame/bg_local.h>
#include <database/database.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_dpvs.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/r_scene_clip.h>
#include <gfx_d3d/r_primarylights.h>
#include <gfx_d3d/r_utils.h>
#include <qcommon/com_bsp.h>
#include <EffectsCore/fx_system.h>
#include <gfx_d3d/r_drawsurf.h>
#include <gfx_d3d/r_workercmds_common.h>
#include <client_mp/client_mp.h>

// Native renderer-owned source metadata. No desktop GPU resources are
// allocated through these globals.
r_global_permanent_t rgp{};
r_globals_t rg{};

// Renderer platform boundary. Gameplay, interpolation and XAnim keep their
// source ownership. This adapter copies draw requests into bounded storage
// that source preparation resolves on this owner before the next update. The
// CPU renderer consumes only the later deep-copied backend command snapshot.
namespace kisak::vita {
namespace {
void RequireSource() {
#if KISAK_VITA_CPU_RUNTIME
    if(VitaCpuCurrentRole()!=VitaCpuRole::Source) {
        VitaCpuProfile_Invalidate("live-source-render-storage-owner-violation");std::abort();
    }
#endif
}
constexpr uint32_t kResourceCapacity = 1024;
VitaRenderPacket packet;
refdef_s view;
VitaSourceUiDraw ui[VitaRenderPacket::kMaxUiCommands];
struct Resource { const void* object; const cpose_t* pose; };
Resource resources[kResourceCapacity];
uint16_t resourceCount = 0;
#if KISAK_VITA_CPU_RUNTIME
VitaCpuPointerIndex<2048> resourceIndex;
#endif
uint32_t frameSequence = 0;
struct Bounds { float mins[3], maxs[3]; bool linked; };
Bounds entities[MAX_GENTITIES];
Bounds dynEntities[2][1024];
float lodOrigin[3];
constexpr uint32_t kFxVertices = 8192, kFxIndices = 24576, kFxDraws = 512;
alignas(16) GfxPackedVertex fxVertices[kFxVertices];
alignas(16) r_double_index_t fxIndices[kFxIndices / 2];
alignas(16) float fxArguments[256][4];
VitaSourceFxDraw fxDraws[kFxDraws];
GfxParticleCloud fxClouds[32];
uint16_t fxCloudMaterials[32];
GfxLight fxLights[32];
uint32_t fxVertexCount = 0, fxIndexCount = 0, fxArgumentCount = 0;
bool fxWriting = false;

uint16_t Ref(const void* object, const cpose_t* pose = nullptr) {
    if (!object) return 0;
#if KISAK_VITA_CPU_RUNTIME
    auto* cell=resourceIndex.Find(reinterpret_cast<uintptr_t>(object),reinterpret_cast<uintptr_t>(pose));
    if(!cell){++packet.rejectedCommands;return 0;}
    if(cell->id)return cell->id;
#else
    for (uint16_t i = 0; i < resourceCount; ++i)
        if (resources[i].object == object && resources[i].pose == pose) return i + 1;
#endif
    if (resourceCount == kResourceCapacity) { ++packet.rejectedCommands; return 0; }
    resources[resourceCount++] = {object, pose};
#if KISAK_VITA_CPU_RUNTIME
    *cell={reinterpret_cast<uintptr_t>(object),reinterpret_cast<uintptr_t>(pose),resourceCount};
#endif
    return resourceCount;
}
VitaSourceUiDraw* Ui(VitaSourceUiKind kind, const void* resource = nullptr) {
    if (packet.uiCommandCount == packet.kMaxUiCommands) {
        ++packet.rejectedCommands; return nullptr;
    }
    const uint32_t index = packet.uiCommandCount++;
    ui[index] = {};
    ui[index].kind = kind;
    ui[index].resource = Ref(resource);
    packet.uiCommands[index] = {static_cast<uint16_t>(index), ui[index].resource};
    return &ui[index];
}
void Rect(VitaSourceUiDraw& cmd, float x, float y, float w, float h) {
    const float corners[4][2]{{x,y},{x+w,y},{x+w,y+h},{x,y+h}};
    std::memcpy(cmd.corners, corners, sizeof(corners));
}
void Color(VitaSourceUiDraw& cmd, const float* color) {
    if (color) std::copy_n(color, 4, cmd.color);
}
void Uv(VitaSourceUiDraw& cmd, float s0, float t0, float s1, float t1) {
    const float uv[4]{s0,t0,s1,t1};
    const float texcoords[4][2]{{s0,t0},{s1,t0},{s1,t1},{s0,t1}};
    std::memcpy(cmd.uv, uv, sizeof(uv));
    std::memcpy(cmd.texcoords, texcoords, sizeof(texcoords));
}
} // namespace

void VitaSourceRender_BeginMap(uint32_t generation) { RequireSource();
    packet = {}; packet.mapGeneration = generation;
    view = {}; resourceCount = 0;
#if KISAK_VITA_CPU_RUNTIME
    resourceIndex.Reset();
#endif
    std::memset(entities, 0, sizeof(entities));
    std::memset(dynEntities, 0, sizeof(dynEntities));
}
const VitaRenderPacket& VitaSourceRender_Packet() { RequireSource(); return packet; }
const refdef_s* VitaSourceRender_View() { RequireSource(); return packet.sourceViewValid ? &view : nullptr; }
const VitaSourceUiDraw* VitaSourceRender_Ui(uint32_t generation, uint32_t index) { RequireSource();
    return generation == packet.frameGeneration && index < packet.uiCommandCount ? &ui[index] : nullptr;
}
const GfxCmdDrawText2D* VitaSourceRender_Text(uint32_t generation, uint32_t index) {
    const auto* command = VitaSourceRender_Ui(generation, index);
    return command && command->kind == VitaSourceUiKind::Text
        ? reinterpret_cast<const GfxCmdDrawText2D*>(command->sourceTextCommand) : nullptr;
}
const void* VitaSourceRender_Resource(uint32_t generation, uint16_t handle) { RequireSource();
    return generation == packet.frameGeneration && handle && handle <= resourceCount ? resources[handle-1].object : nullptr;
}
bool VitaSourceRender_DObj(uint32_t generation, uint16_t handle, const DObj_s** object, const cpose_t** pose) {
    if (!object || !pose || !VitaSourceRender_Resource(generation, handle) || !resources[handle-1].pose) return false;
    *object = static_cast<const DObj_s*>(resources[handle-1].object);
    *pose = resources[handle-1].pose;
    return true;
}
bool VitaSourceRender_Effects(uint32_t generation, VitaSourceFxView* output) { RequireSource();
    if (!output || generation != packet.frameGeneration || fxWriting) return false;
    *output = {fxVertices, reinterpret_cast<const uint16_t*>(fxIndices), fxArguments,
        fxDraws, fxClouds, fxCloudMaterials, fxLights, fxVertexCount, fxIndexCount};
    return true;
}
} // namespace kisak::vita

using namespace kisak::vita;
GfxCmdHeader* R_GetCommandBuffer(GfxRenderCommand kind, int bytes) {
    if (kind != RC_DRAW_TEXT_2D || bytes < static_cast<int>(sizeof(GfxCmdDrawText2D))
        || static_cast<size_t>(bytes) > sizeof(VitaSourceUiDraw::sourceTextCommand)) {
        ++packet.rejectedCommands;
        return nullptr;
    }
    auto* command = Ui(VitaSourceUiKind::Text);
    if (!command) return nullptr;
    auto* header = reinterpret_cast<GfxCmdHeader*>(command->sourceTextCommand);
    header->id = static_cast<uint16_t>(kind);
    header->byteCount = static_cast<uint16_t>(bytes);
    return header;
}
int RB_TextSceneTime() { return packet.sourceServerTime; }
void R_BeginRegistration(vidConfig_t* output) {
    if (!output)
        Com_Error(ERR_DROP, "Vita: missing source video configuration output");
    DvarLimits nearLimits{};
    nearLimits.value.min = 0.001f;
    nearLimits.value.max = 10000.0f;
    Dvar_RegisterFloat("r_znear", R_DEFAULT_NEAR_CLIP, nearLimits, DVAR_CHEAT,
        "Things closer than this aren't drawn.  Reducing this increases z-fighting in the distance.");
    // The native backend and display are already owned by the composition
    // root. Report their fixed configuration to the original client setup.
    *output = {};
    output->sceneWidth = output->displayWidth = 960;
    output->sceneHeight = output->displayHeight = 544;
    output->displayFrequency = 60;
    output->isFullscreen = 1;
    output->aspectRatioWindow = 960.0f / 544.0f;
    output->aspectRatioScenePixel = output->aspectRatioDisplayPixel = 1.0f;
    output->maxTextureSize = 4096;
    output->maxTextureMaps = 16;
    output->deviceSupportsGamma = false; // color grading uses the native shader
    rgp.defaultMaterial = static_cast<Material*>(VitaSourcePlatform_FindAsset(ASSET_TYPE_MATERIAL, "$default"));
}
bool Material_IsDefault(const Material* material) {
    // The typed registry returns null for missing assets and never substitutes
    // a second material with the default texture/technique tables. Identity
    // is therefore sufficient here; desktop pointer-table aliasing is absent.
    const auto* fallback = VitaSourcePlatform_FindAsset(ASSET_TYPE_MATERIAL, "$default");
    return material && fallback && material == fallback;
}
void R_ClearScene(uint32_t) {
    const uint32_t map = packet.mapGeneration;
    if (++frameSequence == 0)
        Com_Error(ERR_DROP, "Vita: source render frame generation exhausted");
    packet = {}; packet.mapGeneration = map; packet.frameGeneration = frameSequence;
    resourceCount = 0;
#if KISAK_VITA_CPU_RUNTIME
    resourceIndex.Reset();
#endif
    fxVertexCount = fxIndexCount = fxArgumentCount = 0;
    fxWriting = false;
}
void R_InitSceneData(int local) { R_ClearScene(local); }
void R_RenderScene(const refdef_s* source) {
    if (!source || !(source->tanHalfFovX > 0) || !(source->tanHalfFovY > 0)
        || !std::isfinite(source->tanHalfFovX) || !std::isfinite(source->tanHalfFovY)) {
        ++packet.rejectedCommands; return;
    }
    view = *source;
    std::memcpy(packet.cameraOrigin, source->vieworg, sizeof(packet.cameraOrigin));
    std::memcpy(packet.cameraAxis, source->viewaxis, sizeof(packet.cameraAxis));
    packet.tanHalfFovX = source->tanHalfFovX; packet.tanHalfFovY = source->tanHalfFovY;
    const dvar_s* znear = Dvar_FindVar("r_znear");
    packet.nearPlane = R_ResolveNearClip(source->zNear,
        znear ? znear->current.value : R_DEFAULT_NEAR_CLIP);
    if (!VitaBuildViewProjection(&packet)) {
        ++packet.rejectedCommands; return;
    }
    packet.sourceServerTime = source->time;
    packet.sourceViewValid = true;
    CL_UpdateColor(0);
}
void R_SetLodOrigin(const refdef_s* source) { if (source) std::copy_n(source->vieworg, 3, lodOrigin); }
double R_GetFarPlaneDist() {
    const dvar_s* zfar = Dvar_FindVar("r_zfar");
    return zfar && zfar->current.value > 0 ? zfar->current.value : std::numeric_limits<float>::max();
}
void R_AddDObjToScene(const DObj_s* obj, const cpose_t* pose, uint32_t entnum, uint32_t flags,
                     float* lightingOrigin, float materialTime) {
    if (!obj || !pose || packet.dynamicEntityCount == packet.kMaxDynamicEntities || entnum > UINT16_MAX) {
        ++packet.rejectedCommands; return;
    }
    auto& cmd = packet.dynamicEntities[packet.dynamicEntityCount++];
    cmd.renderAsset = Ref(obj, pose); cmd.entityNumber = static_cast<uint16_t>(entnum);
    cmd.flags = flags; cmd.materialTime = materialTime;
    std::copy_n(pose->origin, 3, cmd.origin); std::copy_n(pose->angles, 3, cmd.angles);
    std::copy_n(lightingOrigin ? lightingOrigin : pose->origin, 3, cmd.lightingOrigin);
}
void R_AddBrushModelToSceneFromAngles(const GfxBrushModel* model, const float* origin, const float* angles, uint16_t entnum) {
    if (!model || !origin || !angles || packet.dynamicEntityCount == packet.kMaxDynamicEntities) {
        ++packet.rejectedCommands; return;
    }
    auto& cmd = packet.dynamicEntities[packet.dynamicEntityCount++];
    cmd.brushModel = true; cmd.renderAsset = Ref(model); cmd.entityNumber = entnum;
    std::copy_n(origin, 3, cmd.origin); std::copy_n(angles, 3, cmd.angles);
}
void R_FilterXModelIntoScene(const XModel* model, const GfxScaledPlacement* placement,
                            uint16_t flags, uint16_t*) {
    if (!model || !placement || packet.dynamicEntityCount == packet.kMaxDynamicEntities) {
        ++packet.rejectedCommands; return;
    }
    auto& cmd = packet.dynamicEntities[packet.dynamicEntityCount++];
    cmd.rigidModel = true; cmd.renderAsset = Ref(model); cmd.entityNumber = ENTITYNUM_NONE;
    cmd.scale = placement->scale; cmd.flags = flags;
    std::copy_n(placement->base.origin,3,cmd.origin);
    std::copy_n(placement->base.quat,4,cmd.quaternion);
}
void R_LinkDObjEntity(uint32_t, uint32_t entnum, float* origin, float radius) {
    if (entnum >= MAX_GENTITIES || !origin) { ++packet.rejectedCommands; return; }
    for (int axis = 0; axis < 3; ++axis) { entities[entnum].mins[axis] = origin[axis]-radius; entities[entnum].maxs[axis] = origin[axis]+radius; }
    entities[entnum].linked = true;
}
void R_LinkBModelEntity(uint32_t, uint32_t entnum, GfxBrushModel* model) {
    if (entnum >= MAX_GENTITIES || !model) { ++packet.rejectedCommands; return; }
    std::copy_n(model->writable.mins,3,entities[entnum].mins); std::copy_n(model->writable.maxs,3,entities[entnum].maxs); entities[entnum].linked=true;
}
void R_UnlinkEntity(uint32_t, uint32_t entnum) { if (entnum < MAX_GENTITIES) entities[entnum].linked = false; }
void R_LinkDynEnt(uint32_t id, DynEntityDrawType type, float* mins, float* maxs) {
    if (static_cast<unsigned>(type) >= 2 || id >= 1024 || !mins || !maxs) { ++packet.rejectedCommands; return; }
    auto& bounds = dynEntities[type][id]; bounds.linked = true;
    std::copy_n(mins,3,bounds.mins); std::copy_n(maxs,3,bounds.maxs);
}
void R_UnlinkDynEnt(uint32_t id, DynEntityDrawType type) {
    if (static_cast<unsigned>(type) < 2 && id < 1024) dynEntities[type][id].linked = false;
}
void R_ClearShadowedPrimaryLightHistory(int) { /* Real-time world shadows are excluded by the slice contract. */ }
void R_InitPrimaryLights(GfxLight* lights) {
    auto* world = static_cast<GfxWorld*>(VitaSourcePlatform_FindAsset(ASSET_TYPE_GFXWORLD, "__active__"));
    if (!world || !lights || world->primaryLightCount > 255 || world->primaryLightCount > comWorld.primaryLightCount)
        Com_Error(ERR_DROP, "Vita: primary-light source closure is invalid");
    for (uint32_t i=0; i<world->primaryLightCount; ++i) {
        const auto& in = comWorld.primaryLights[i]; auto& out=lights[i]; out={};
        out.type=in.type; out.canUseShadowMap=false;
        std::copy_n(in.color,3,out.color); std::copy_n(in.dir,3,out.dir); std::copy_n(in.origin,3,out.origin);
        out.radius=in.radius; out.cosHalfFovOuter=in.cosHalfFovOuter; out.cosHalfFovInner=in.cosHalfFovInner; out.exponent=in.exponent;
        if (in.defName) out.def = static_cast<GfxLightDef*>(VitaSourcePlatform_FindAsset(ASSET_TYPE_LIGHT_DEF,in.defName));
    }
    if (world->sunPrimaryLightIndex && world->sunLight && world->sunPrimaryLightIndex < world->primaryLightCount)
        lights[world->sunPrimaryLightIndex]=*world->sunLight;
}
void R_AddCmdDrawQuadPic(const float (*verts)[2], const float* color, Material* material) {
    if (!verts) return;
    if (auto* cmd=Ui(VitaSourceUiKind::Quad,material)) { std::memcpy(cmd->corners,verts,sizeof(cmd->corners)); Color(*cmd,color); }
}
void R_AddCmdDrawStretchPic(float x,float y,float w,float h,float s0,float t0,float s1,float t1,const float* color,Material* material) {
    if (auto* cmd=Ui(VitaSourceUiKind::Quad,material)) { Rect(*cmd,x,y,w,h); Uv(*cmd,s0,t0,s1,t1); Color(*cmd,color); }
}
void R_AddCmdDrawStretchPicRotateXY(float x,float y,float w,float h,float s0,float t0,float s1,float t1,float angle,const float* color,Material* material) {
    auto* cmd=Ui(VitaSourceUiKind::Quad,material); if(!cmd) return;
    Rect(*cmd,x,y,w,h); Uv(*cmd,s0,t0,s1,t1); Color(*cmd,color);
    const float rad=angle*0.017453292519943295f, c=std::cos(rad), s=std::sin(rad), cx=x+w*0.5f,cy=y+h*0.5f;
    for(auto& p:cmd->corners) { const float dx=p[0]-cx,dy=p[1]-cy; p[0]=cx+dx*c-dy*s;p[1]=cy+dx*s+dy*c; }
}
void R_AddCmdDrawStretchPicFlipST(float x,float y,float w,float h,float s0,float t0,float s1,float t1,const float* color,Material* material) {
    auto* cmd=Ui(VitaSourceUiKind::Quad,material); if(!cmd) return;
    Rect(*cmd,x,y,w,h); Color(*cmd,color); Uv(*cmd,s0,t0,s1,t1);
    std::swap(cmd->texcoords[1][0],cmd->texcoords[3][0]); std::swap(cmd->texcoords[1][1],cmd->texcoords[3][1]);
}
void R_AddCmdDrawStretchPicRotateST(float x,float y,float w,float h,float cs,float ct,float radius,float ss,float st,float angle,const float* color,Material* material) {
    auto* cmd=Ui(VitaSourceUiKind::Quad,material); if(!cmd) return;
    Rect(*cmd,x,y,w,h); Color(*cmd,color);
    const float rad=angle*0.017453292519943295f,c=std::cos(rad)*radius,s=std::sin(rad)*radius;
    const float offsets[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
    for(int i=0;i<4;++i){cmd->texcoords[i][0]=cs+(offsets[i][0]*c-offsets[i][1]*s)*ss;cmd->texcoords[i][1]=ct+(offsets[i][0]*s+offsets[i][1]*c)*st;}
}
void R_AddCmdProjectionSet2D() { /* All queued UI coordinates are source physical screen pixels. */ }
void R_AddCmdSaveScreen(uint32_t index) { if(auto* cmd=Ui(VitaSourceUiKind::SaveScreen)){Rect(*cmd,0,0,960,544);cmd->screenIndex=index;} }
void R_AddCmdSaveScreenSection(float x,float y,float w,float h,uint32_t index){if(auto*cmd=Ui(VitaSourceUiKind::SaveScreen)){Rect(*cmd,x,y,w,h);cmd->screenIndex=index;}}
void R_AddCmdBlendSavedScreenShockBlurred(int fade,float x,float y,float w,float h,uint32_t index){if(auto*cmd=Ui(VitaSourceUiKind::ShockBlur)){Rect(*cmd,x,y,w,h);cmd->effectTimes[0]=fade;cmd->screenIndex=index;}}
void R_AddCmdBlendSavedScreenShockFlashed(float white,float grab,float x,float y,float w,float h){if(auto*cmd=Ui(VitaSourceUiKind::ShockFlash)){Rect(*cmd,x,y,w,h);cmd->color[3]=white;cmd->glowColor[3]=grab;}}

// Cgame's source update phases remain on the main thread. EffectsCore owns
// every lifetime, random sample, trajectory and vertex; only its storage and
// draw submission are adapted here.
void R_UpdateNonDependentEffects(FxCmd* cmd) { FX_UpdateNonDependent(cmd); }
void R_UpdateSpotLightEffect(FxCmd* cmd) { FX_UpdateSpotLight(cmd); }
void R_UpdateRemainingEffects(FxCmd* cmd) {
    FX_UpdateRemaining(cmd);
    FX_EndUpdate(cmd->localClientNum);
    FX_AddNonSpriteDrawSurfs(cmd);
    FxGenerateVertsCmd generate{};
    FX_FillGenerateVertsCmd(cmd->localClientNum,&generate);
    FX_GenerateVerts(&generate);
}
void R_BeginCodeMeshVerts() { fxWriting=true; }
void R_EndCodeMeshVerts() { fxWriting=false; }
char R_ReserveCodeMeshVerts(int count,uint16_t* base) {
    if (!fxWriting || !base || count<0 || static_cast<uint32_t>(count)>kFxVertices-fxVertexCount) {
        ++packet.rejectedCommands; return 0;
    }
    *base=static_cast<uint16_t>(fxVertexCount); fxVertexCount+=count; return 1;
}
char R_ReserveCodeMeshIndices(int count,r_double_index_t** indices) {
    if (!fxWriting || !indices || count<0 || (count&1) || static_cast<uint32_t>(count)>kFxIndices-fxIndexCount) {
        ++packet.rejectedCommands; return 0;
    }
    *indices=fxIndices+fxIndexCount/2; fxIndexCount+=count; return 1;
}
char R_ReserveCodeMeshArgs(int count,uint32_t* offset) {
    if(!fxWriting || !offset || count<0 || static_cast<uint32_t>(count)>256-fxArgumentCount) {
        ++packet.rejectedCommands; return 0;
    }
    *offset=fxArgumentCount;fxArgumentCount+=count;return 1;
}
GfxPackedVertex* R_GetCodeMeshVerts(uint16_t base) {
    if(!fxWriting || base>=fxVertexCount) Com_Error(ERR_DROP,"Vita: invalid source effect vertex range");
    return fxVertices+base;
}
float (*R_GetCodeMeshArgs(uint32_t offset))[4] {
    if(!fxWriting || offset>=fxArgumentCount) Com_Error(ERR_DROP,"Vita: invalid source effect argument range");
    return fxArguments+offset;
}
void R_AddCodeMeshDrawSurf(Material* material,r_double_index_t* indices,uint32_t count,uint32_t argOffset,uint32_t argCount,const char*) {
    const uintptr_t address=reinterpret_cast<uintptr_t>(indices), base=reinterpret_cast<uintptr_t>(fxIndices);
    if(!material || address<base || ((address-base)&3) || (address-base)/2>fxIndexCount
        || count>fxIndexCount-(address-base)/2 || argOffset>fxArgumentCount
        || argCount>fxArgumentCount-argOffset || packet.effectDrawCount==kFxDraws) {
        ++packet.rejectedCommands;return;
    }
    fxDraws[packet.effectDrawCount++]={Ref(material),static_cast<uint16_t>(argOffset),
        static_cast<uint16_t>(argCount),static_cast<uint32_t>((address-base)/2),count};
}
GfxParticleCloud* R_AddParticleCloudToScene(Material* material) {
    if(!material || packet.effectCloudCount==32){++packet.rejectedCommands;return nullptr;}
    const auto index=packet.effectCloudCount++;
    fxCloudMaterials[index]=Ref(material);fxClouds[index]={};return &fxClouds[index];
}
void R_AddSpotLightToScene(const float* origin,const float* dir,float radius,float r,float g,float b) {
    if(!origin || packet.effectLightCount==32){++packet.rejectedCommands;return;}
    auto& light=fxLights[packet.effectLightCount++];light={};
    light.type=dir?2:3;std::copy_n(origin,3,light.origin);
    if(dir)std::copy_n(dir,3,light.dir);
    light.radius=radius;light.color[0]=r;light.color[1]=g;light.color[2]=b;
}
void R_AddOmniLightToScene(const float* origin,float radius,float r,float g,float b) {R_AddSpotLightToScene(origin,nullptr,radius,r,g,b);}
void R_SyncGpu(int(*)(uint64_t)) {
    // Cgame queues CPU records. The Vita backend owns GXM fences when acquiring
    // dynamic buffers and presenting; this point never reuses GPU storage.
}

