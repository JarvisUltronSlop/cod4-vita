#include <vita/vita_source_model_backend.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_profile.h>
#include <vita/vita_skin_math.h>
#include <vita/vita_source_vertex.h>
#include <vita/vita_skin_blocks.h>
#endif
#include <vita/vita_source_render.h>
#include <vita/vita_model_bundle.h>
#include <cgame_mp/cg_local_mp.h>
#include <gfx_d3d/r_model_skin.h>
#include <qcommon/com_pack.h>
#include <xanim/dobj.h>
#include <xanim/xmodel.h>
#include <xanim/xanim.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <array>
#if KISAK_VITA_PROFILE
#include <psp2/kernel/clib.h>
#include <cstdio>
#include <cfloat>
#endif

namespace kisak::vita {
namespace {
// Reused for one source surface at a time and released on map teardown.
// LOD0 viewmodel geometry is validated before it reaches this boundary.
constexpr uint32_t kMaxSurfaceVertices = 8192;
#if KISAK_VITA_CPU_RUNTIME
constexpr uint32_t kSkinBlockVertices = 64;
alignas(16) std::array<GfxPackedVertex,kSkinBlockVertices> scratch;
#else
std::vector<GfxPackedVertex> scratch;
#endif
#if KISAK_VITA_PROFILE
uint32_t capturedModels = 0;
#endif
}
void VitaSourceModel_Reset() {
#if !KISAK_VITA_CPU_RUNTIME
    std::vector<GfxPackedVertex>().swap(scratch);
#endif
#if KISAK_VITA_PROFILE
    capturedModels = 0;
#endif
}

static bool SkinModel(uint32_t generation, uint16_t handle,
    uint32_t modelIndex, const VitaModelRecord& mesh, GfxDrawVertex* output,
    uint32_t capacity, VitaSourceModelResult* result, void* packedOutput) {
#if KISAK_VITA_CPU_RUNTIME
    VITA_CPU_SCOPE("source-native-skin");
#endif
    const auto& packet = VitaSourceRender_Packet();
    const auto* view = VitaSourceRender_View();
    const DObj_s* object = nullptr;
    const cpose_t* pose = nullptr;
    if ((!output && !packedOutput) || !result || !view || !packet.sourceViewValid
        || !VitaSourceRender_DObj(generation, handle, &object, &pose)
        || modelIndex >= object->numModels || !object->models
        || capacity < mesh.vertices.size() || mesh.surfaces.size() > 64)
        return false;
    const XModel* model = object->models[modelIndex];
    if (!model || !model->name || mesh.name != model->name
        || model->numsurfs != mesh.surfaces.size() || !model->surfs
        || !model->baseMat || model->numBones != mesh.bones.size()) return false;
    uint32_t boneOffset = 0;
    for (uint32_t i = 0; i < modelIndex; ++i) boneOffset += object->models[i]->numBones;
    if (boneOffset + model->numBones > object->numBones
        || object->numBones > DOBJ_MAX_PARTS) return false;
    int partBits[4]{};
    for (uint32_t bone = 0; bone < object->numBones; ++bone)
        partBits[bone >> 5] |= 0x80000000u >> (bone & 31);
    const DObjAnimMat* animated;
    {
#if KISAK_VITA_CPU_RUNTIME
        VITA_CPU_SCOPE("source-skin-pose");
#endif
        animated = CG_DObjCalcPose(pose, object, partBits);
    }
    if (!animated) return false;
    alignas(16) DObjSkelMat skin[DOBJ_MAX_PARTS]{};
    {
#if KISAK_VITA_CPU_RUNTIME
    VITA_CPU_SCOPE("source-skin-bone-matrices");
#endif
    for (uint32_t bone = 0; bone < model->numBones; ++bone) {
        DObjSkelMat inverseBind{}, current{};
        ConvertQuatToInverseSkelMat(&model->baseMat[bone], &inverseBind);
        ConvertQuatToSkelMat(&animated[boneOffset + bone], &current);
        R_MultiplySkelMat(&inverseBind, &current, &skin[bone]);
        skin[bone].origin[3] = 1;
    }
    }
    *result = {};
#if KISAK_VITA_CPU_RUNTIME
    std::memcpy(result->projection.viewOffset,view->viewOffset,sizeof(result->projection.viewOffset));
    std::memcpy(result->projection.cameraOrigin,packet.cameraOrigin,sizeof(result->projection.cameraOrigin));
    std::memcpy(result->projection.cameraAxis,packet.cameraAxis,sizeof(result->projection.cameraAxis));
    result->projection.tanHalfFovX=packet.tanHalfFovX;
    result->projection.tanHalfFovY=packet.tanHalfFovY;
    static_assert(sizeof(VitaPackedSkinVertex)==sizeof(GfxPackedVertex));
    static_assert(offsetof(VitaPackedSkinVertex,color)==offsetof(GfxPackedVertex,color));
    static_assert(offsetof(VitaPackedSkinVertex,texCoord)==offsetof(GfxPackedVertex,texCoord));
    static_assert(offsetof(VitaPackedSkinVertex,normal)==offsetof(GfxPackedVertex,normal));
    static_assert(offsetof(VitaPackedSkinVertex,tangent)==offsetof(GfxPackedVertex,tangent));
#endif
    const auto Project = [&](const float relative[3], float projected[3], float world[3]) {
        float delta[3];
        for (int axis = 0; axis < 3; ++axis) {
            world[axis] = relative[axis] + view->viewOffset[axis];
            delta[axis] = world[axis] - packet.cameraOrigin[axis];
        }
#if KISAK_VITA_CPU_RUNTIME
        projected[0] = -skinmath::Vec3Dot(delta, packet.cameraAxis[1]) / packet.tanHalfFovX;
        projected[1] = skinmath::Vec3Dot(delta, packet.cameraAxis[2]) / packet.tanHalfFovY;
        projected[2] = skinmath::Vec3Dot(delta, packet.cameraAxis[0]);
#else
        projected[0] = -Vec3Dot(delta, packet.cameraAxis[1]) / packet.tanHalfFovX;
        projected[1] = Vec3Dot(delta, packet.cameraAxis[2]) / packet.tanHalfFovY;
        projected[2] = Vec3Dot(delta, packet.cameraAxis[0]);
#endif
    };
    for (uint32_t index = 0; index < model->numsurfs; ++index) {
        const XSurface& surface = model->surfs[index];
        const auto& layout = mesh.surfaces[index];
        if (!surface.verts0 || surface.vertCount != layout.vertexCount
            || surface.vertCount > kMaxSurfaceVertices
            || static_cast<uint64_t>(layout.vertexStart) + surface.vertCount > capacity)
            return false;
        for (uint32_t bone = 0; bone < model->numBones; ++bone) {
            const uint32_t sourceBit = 0x80000000u >> (bone & 31);
            const uint32_t objectBone = boneOffset + bone;
            if ((surface.partBits[bone >> 5] & sourceBit)
                && (object->hidePartBits[objectBone >> 5] & (0x80000000u >> (objectBone & 31)))) {
                result->hiddenSurfaces |= uint64_t{1} << index;
                break;
            }
        }
#if !KISAK_VITA_CPU_RUNTIME
        scratch.resize(surface.vertCount);
#endif
#if KISAK_VITA_CPU_RUNTIME
        {
            VITA_CPU_SCOPE(packedOutput ? "source-skin-packed-blocks" : "source-skin-convert-blocks");
            if (!SkinSurfaceBlocks<XSurface, XSurfaceVertexInfo, XRigidVertList>(surface,
                    kSkinBlockVertices,
                    [&](const XSurface& block) {
                        R_SkinXSurfaceSkinned(&block, skin, scratch.data());
                    },
                    [&](uint32_t first, uint32_t count) {
                        if (packedOutput) {
                            auto* dest=static_cast<VitaPackedSkinVertex*>(packedOutput);
                            std::memcpy(dest+layout.vertexStart+first,scratch.data(),count*sizeof(GfxPackedVertex));
                            return true;
                        }
                        for (uint32_t vertex = 0; vertex < count; ++vertex)
                            if (!ConvertSourceVertex(scratch[vertex],
                                    output[layout.vertexStart + first + vertex], Project)) return false;
                        return true;
                    })) return false;
        }
#else
        {
#if KISAK_VITA_CPU_RUNTIME
            VITA_CPU_SCOPE("source-skin-surface");
#endif
            R_SkinXSurfaceSkinned(&surface, skin, scratch.data());
        }
        {
#if KISAK_VITA_CPU_RUNTIME
        VITA_CPU_SCOPE("source-skin-vertex-conversion");
#endif
        for (uint32_t vertex = 0; vertex < surface.vertCount; ++vertex) {
            const auto& input = scratch[vertex];
            auto& dest = output[layout.vertexStart + vertex];
#if KISAK_VITA_CPU_RUNTIME
            if (!ConvertSourceVertex(input, dest, Project)) return false;
#else
            dest = {};
            Project(input.xyz, dest.position, dest.worldPosition);
            for (float component : dest.position) if (!std::isfinite(component)) return false;
#if KISAK_VITA_CPU_RUNTIME
            skinmath::Vec2UnpackTexCoords(input.texCoord, dest.texCoord);
            skinmath::Vec3UnpackUnitVec(input.normal, dest.normal);
            skinmath::Vec3UnpackUnitVec(input.tangent, dest.tangent);
#else
            Vec2UnpackTexCoords(input.texCoord, dest.texCoord);
            Vec3UnpackUnitVec(input.normal, dest.normal);
            Vec3UnpackUnitVec(input.tangent, dest.tangent);
#endif
            for (int component = 0; component < 4; ++component)
                dest.color[component] = input.color.array[component] / 255.0f;
            dest.shadowVisibility = 1;
#endif
        }
        }
#endif
    }
    if (mesh.flashTagBone >= 0 && mesh.flashTagBone < model->numBones) {
        float projected[3]{}, world[3]{};
        Project(animated[boneOffset + mesh.flashTagBone].trans, projected, world);
        if (projected[2] > 0.1f) {
            result->muzzle[0] = projected[0] / projected[2];
            result->muzzle[1] = projected[1] / projected[2];
            result->muzzleValid = std::isfinite(result->muzzle[0]) && std::isfinite(result->muzzle[1]);
        }
    }
#if KISAK_VITA_PROFILE
    // Bounded diagnostic of the original source pose before GPU submission.
    // This does not replace or modify any vertex, bone, or material state.
    if (output && packet.sourceServerTime >= 21000 && modelIndex < 32
        && !(capturedModels & (1u << modelIndex))) {
        capturedModels |= 1u << modelIndex;
        float minimum[3]{FLT_MAX, FLT_MAX, FLT_MAX};
        float maximum[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        uint32_t inFront = 0, inViewport = 0;
        for (uint32_t index = 0; index < mesh.vertices.size(); ++index) {
            const auto& vertex = output[index];
            for (int axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
                maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
            }
            if (vertex.position[2] > 0.1f) {
                ++inFront;
                if (std::fabs(vertex.position[0]) <= vertex.position[2]
                    && std::fabs(vertex.position[1]) <= vertex.position[2]) ++inViewport;
            }
        }
        sceClibPrintf("[kisak-vita] source skin bounds model=%s time=%d vertices=%u front=%u viewport=%u min=%.4f,%.4f,%.4f max=%.4f,%.4f,%.4f pose=%.4f,%.4f,%.4f offset=%.4f,%.4f,%.4f camera=%.4f,%.4f,%.4f root=%.4f,%.4f,%.4f\n",
            model->name, packet.sourceServerTime, static_cast<uint32_t>(mesh.vertices.size()), inFront, inViewport,
            minimum[0], minimum[1], minimum[2], maximum[0], maximum[1], maximum[2],
            pose->origin[0], pose->origin[1], pose->origin[2], view->viewOffset[0], view->viewOffset[1], view->viewOffset[2],
            packet.cameraOrigin[0], packet.cameraOrigin[1], packet.cameraOrigin[2],
            animated[boneOffset].trans[0], animated[boneOffset].trans[1], animated[boneOffset].trans[2]);
    }
#endif
    return true;
}
bool VitaSourceModel_Skin(uint32_t generation,uint16_t handle,uint32_t modelIndex,
    const VitaModelRecord& mesh,GfxDrawVertex* output,uint32_t capacity,VitaSourceModelResult* result) {
    return SkinModel(generation,handle,modelIndex,mesh,output,capacity,result,nullptr);
}
#if KISAK_VITA_CPU_RUNTIME
bool VitaSourceModel_SkinPacked(uint32_t generation,uint16_t handle,uint32_t modelIndex,
    const VitaModelRecord& mesh,VitaPackedSkinVertex* output,uint32_t capacity,VitaSourceModelResult* result) {
    return SkinModel(generation,handle,modelIndex,mesh,nullptr,capacity,result,output);
}
#endif
} // namespace kisak::vita
