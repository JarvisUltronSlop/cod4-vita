#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_skin_math.h>
#include <vita/vita_source_vertex.h>
#include <vita/vita_skin_blocks.h>
#include <vita/vita_viewmodel_packet.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <limits>
#include <vector>
#ifdef _MSC_VER
#define SKIN_NOINLINE __declspec(noinline)
#else
#define SKIN_NOINLINE __attribute__((noinline))
#define __cdecl
#endif
#define iassert(x) assert(x)
#define PROF_SCOPED(x)
#define HIWORD(x) (uint32_t(uint16_t((x) >> 16)))
#define LOWORD(x) (uint32_t(uint16_t(x)))
#define LODWORD(x) (*reinterpret_cast<uint32_t*>(&(x)))
using vec3r = const float*;
union PackedUnitVec { uint32_t packed; uint8_t array[4]; };
union PackedTexCoords { uint32_t packed; };
union GfxColor { uint32_t packed; uint8_t array[4]; };
struct GfxPackedVertex {
    float xyz[3], binormalSign;
    GfxColor color;
    PackedTexCoords texCoord;
    PackedUnitVec normal, tangent;
};
struct DObjSkelMat { float axis[3][4], origin[4]; };
struct XSurfaceVertexInfo { int16_t vertCount[4]; uint16_t* vertsBlend; };
struct XRigidVertList { uint16_t boneOffset, vertCount; };
struct XSurface {
    bool deformed;
    uint16_t vertCount;
    GfxPackedVertex* verts0;
    XSurfaceVertexInfo vertInfo;
    uint32_t vertListCount;
    XRigidVertList* vertList;
};
static_assert(sizeof(GfxPackedVertex) == 32 && sizeof(DObjSkelMat) == 64);
#include "source_skin_math.inc"

int main() {
    std::mt19937 rng(0xc0d417);
    uint64_t comparisons = 0;
    uint64_t conversionComparisons = 0;
    uint64_t blockComparisons = 0;
    auto project = [](const float* input, float* projected, float* world) {
        const float offset[3]={12.75f,-23.5f,1.25f};
        const float camera[3]={-4.5f,6.125f,0.75f};
        const float axis[3][3]={{0.6f,0.8f,0},{-0.8f,0.6f,0},{0,0,1}};
        float delta[3];
        for (int a=0; a<3; ++a) { world[a]=input[a]+offset[a]; delta[a]=world[a]-camera[a]; }
        projected[0]=-reference::Vec3Dot(delta,axis[1])/0.67f;
        projected[1]=reference::Vec3Dot(delta,axis[2])/0.53f;
        projected[2]=reference::Vec3Dot(delta,axis[0]);
    };
    const kisak::vita::VitaViewModelProjection copiedProjection{
        {12.75f,-23.5f,1.25f},{-4.5f,6.125f,0.75f},
        {{0.6f,0.8f,0},{-0.8f,0.6f,0},{0,0,1}},0.67f,0.53f};
    auto checkConversion = [&](const GfxPackedVertex& input) {
        GfxDrawVertex old, fast;
        std::memset(&old,0xa5,sizeof(old)); std::memset(&fast,0x5a,sizeof(fast));
        const bool valid=reference::ConvertOriginal(input,old,project);
        assert(kisak::vita::ConvertSourceVertex(input,fast,project)==valid);
        kisak::vita::VitaPackedSkinVertex copied;
        std::memcpy(&copied,&input,sizeof(copied));
        GfxDrawVertex moved;
        assert(kisak::vita::ConvertSourceVertex(copied,moved,copiedProjection)==valid);
        assert(std::memcmp(&old,&moved,sizeof(old))==0);
        assert(std::memcmp(&old,&fast,sizeof(old))==0);
        ++conversionComparisons;
    };
    // Exhaust all scale/component byte combinations in packed basis decoding,
    // and every 16-bit UV encoding in each component, including signed zero.
    for (uint32_t i = 0; i < 65536; ++i) {
        PackedUnitVec packed{(i >> 8) << 24 | (i & 255) * 0x010101u};
        float old[3]{}, fast[3]{};
        reference::Vec3UnpackUnitVec(packed, old);
        candidate::Vec3UnpackUnitVec(packed, fast);
        assert(std::memcmp(old, fast, sizeof(old)) == 0);
        PackedTexCoords uv{(i << 16) | (65535 - i)};
        reference::Vec2UnpackTexCoords(uv, old);
        candidate::Vec2UnpackTexCoords(uv, fast);
        assert(std::memcmp(old, fast, sizeof(old)) == 0);
        comparisons += 2;
        GfxPackedVertex vertex{};
        vertex.normal=packed; vertex.tangent=packed; vertex.texCoord=uv;
        vertex.color.packed=(i & 255)*0x01010101u;
        checkConversion(vertex);
    }
    constexpr uint32_t count = 256, bones = 32;
    alignas(16) std::array<GfxPackedVertex, count + 2> original{}, optimized{};
    std::array<GfxPackedVertex, count> vertices{};
    alignas(16) std::array<DObjSkelMat, bones> matrices{};
    std::vector<uint16_t> blend;
    std::array<XRigidVertList, 4> rigid{};
    XSurface surface{};
    surface.verts0 = vertices.data();
    surface.vertCount = count;
    auto finite = [&]() { return (int(rng() % 2000001) - 1000000) / 1000000.0f; };
    for (uint32_t trial = 0; trial < 2500; ++trial) {
        for (auto& m : matrices) {
            // Bounded basis avoids undefined float->int packing on arbitrary
            // nonfinite/overflow inputs. Source input validation is unchanged.
            for (auto& axis : m.axis) for (auto& x : axis) x = finite() * 0.25f;
            for (auto& x : m.origin) x = finite() * 10000;
        }
        for (auto& v : vertices) {
            for (auto& x : v.xyz) x = finite() * 10000;
            v.binormalSign = rng() & 1 ? 1.0f : -1.0f;
            v.color.packed = rng(); v.texCoord.packed = rng();
            // Alternate the usual scale byte with arbitrary packed bases, so
            // full surface output covers all scale-table entries as well as
            // the exhaustive component/scale checks above.
            v.normal.packed = trial & 1 ? rng() : (rng() & 0xffffff) | 0x3f000000;
            v.tangent.packed = trial & 1 ? rng() : (rng() & 0xffffff) | 0x3f000000;
            const float oldDot = reference::Vec3Dot(v.xyz, matrices[0].axis[0]);
            const float fastDot = candidate::Vec3Dot(v.xyz, matrices[0].axis[0]);
            assert(std::memcmp(&oldDot, &fastDot, sizeof(oldDot)) == 0);
            ++comparisons;
            checkConversion(v);
        }
        surface.deformed = trial % 6 != 0;
        blend.clear();
        for (int weightClass = 0; weightClass < 4; ++weightClass) {
            surface.vertInfo.vertCount[weightClass] = trial % 5 == 4 ? count / 4 : (trial % 4 == weightClass ? count : 0);
            for (int v = 0; v < surface.vertInfo.vertCount[weightClass]; ++v) {
                blend.push_back(uint16_t((rng() % bones) * sizeof(DObjSkelMat)));
                for (int extra = 0; extra < weightClass; ++extra) {
                    blend.push_back(uint16_t((rng() % bones) * sizeof(DObjSkelMat)));
                    blend.push_back(trial % 7 == 0 ? uint16_t(extra & 1 ? 65535 : 0) : uint16_t(rng()));
                }
            }
        }
        surface.vertInfo.vertsBlend = blend.data();
        for (auto& r : rigid) { r.boneOffset = uint16_t((rng() % bones) * sizeof(DObjSkelMat)); r.vertCount = count / 4; }
        surface.vertList = rigid.data(); surface.vertListCount = rigid.size();
        std::memset(original.data(), 0xa5, sizeof(original));
        optimized = original;
        reference::R_SkinXSurfaceSkinned(&surface, matrices.data(), original.data() + 1);
        candidate::R_SkinXSurfaceSkinned(&surface, matrices.data(), optimized.data() + 1);
        assert(std::memcmp(original.data(), optimized.data(), sizeof(original)) == 0);
        // Guards checked independently, not just by differential comparison.
        std::array<uint8_t, sizeof(GfxPackedVertex)> guard; guard.fill(0xa5);
        assert(std::memcmp(original.data(), guard.data(), guard.size()) == 0);
        assert(std::memcmp(original.data() + count + 1, guard.data(), guard.size()) == 0);
        for (uint32_t blockSize : {7u, 64u, 255u}) {
            alignas(16) std::array<GfxPackedVertex, 257> blockScratch;
            std::memset(blockScratch.data(), 0xa5, sizeof(blockScratch));
            uint32_t consumed = 0;
            const bool ok = kisak::vita::SkinSurfaceBlocks<XSurface, XSurfaceVertexInfo, XRigidVertList>(
                surface, blockSize,
                [&](const XSurface& block) {
                    assert(block.vertCount <= blockSize);
                    candidate::R_SkinXSurfaceSkinned(&block, matrices.data(), blockScratch.data() + 1);
                }, [&](uint32_t first, uint32_t amount) {
                    assert(first == consumed);
                    for (uint32_t v = 0; v < amount; ++v) {
                        assert(std::memcmp(&original[first+v+1], &blockScratch[v+1], sizeof(GfxPackedVertex)) == 0);
                        GfxDrawVertex expected, actual;
                        std::memset(&expected, 0xa5, sizeof(expected)); actual = expected;
                        assert(reference::ConvertOriginal(original[first+v+1], expected, project)
                            == kisak::vita::ConvertSourceVertex(blockScratch[v+1], actual, project));
                        assert(std::memcmp(&expected, &actual, sizeof(expected)) == 0);
                        ++blockComparisons;
                    }
                    consumed += amount;
                    return true;
                });
            assert(ok && consumed == count);
            assert(std::memcmp(blockScratch.data(), guard.data(), guard.size()) == 0);
            assert(std::memcmp(blockScratch.data()+256, guard.data(), guard.size()) == 0);
        }
        comparisons += count;
    }
    // Empty weighted and rigid surfaces must leave their outputs untouched.
    surface.deformed = true; surface.vertInfo = {}; surface.vertCount = 0;
    optimized = original;
    candidate::R_SkinXSurfaceSkinned(&surface, matrices.data(), optimized.data());
    assert(std::memcmp(original.data(), optimized.data(), sizeof(original)) == 0);
    uint32_t skinCalls = 0, consumeCalls = 0;
    auto skinBlock = [&](const XSurface&) { ++skinCalls; };
    auto rejectBlock = [&](uint32_t, uint32_t) { ++consumeCalls; return false; };
    auto runBlocks = [&](const XSurface& input, uint32_t capacity) {
        return kisak::vita::SkinSurfaceBlocks<XSurface, XSurfaceVertexInfo, XRigidVertList>(
            input, capacity, skinBlock, rejectBlock);
    };
    assert(runBlocks(surface, 64)); // Empty weighted surface emits nothing.
    assert(skinCalls == 0 && consumeCalls == 0);
    assert(!runBlocks(surface, 0) && !runBlocks(surface, 32768));
    surface.vertCount = 2; surface.vertInfo.vertCount[0] = 1;
    assert(!runBlocks(surface, 64)); // Invalid totals fail before writes.
    surface.vertInfo.vertCount[0] = -1;
    assert(!runBlocks(surface, 64));
    surface.vertInfo.vertCount[0] = 2;
    assert(!runBlocks(surface, 64)); // Missing blend data.
    uint16_t testBlend[2]{}; surface.vertInfo.vertsBlend = testBlend;
    assert(!runBlocks(surface, 1)); // Consumer failure stops before next block.
    assert(skinCalls == 1 && consumeCalls == 1);
    surface.vertInfo = {}; surface.vertCount = 0;
    for (float special : {0.0f,-0.0f,std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::max()}) {
        for (int axis=0; axis<3; ++axis) {
            auto v=vertices[0]; v.xyz[axis]=special; checkConversion(v);
        }
    }
    std::printf("%llu full 84-byte vertex conversion comparisons, including nonfinite failure output and all byte colours; table bytes=%zu\n", (unsigned long long)conversionComparisons, sizeof(kisak::vita::sourceVertexColors));
    surface.deformed = false; surface.vertListCount = 0;
    assert(runBlocks(surface, 64));
    surface.vertListCount = 1; surface.vertList = nullptr;
    assert(!runBlocks(surface, 64));
    surface.vertListCount = 0;
    candidate::R_SkinXSurfaceSkinned(&surface, matrices.data(), optimized.data());
    assert(std::memcmp(original.data(), optimized.data(), sizeof(original)) == 0);
    std::printf("%llu exact comparisons: packed UV/basis and full scalar skin vertices, rigid and 1-4 weights, mixed groups, zero/max weights, guards and empty surfaces\n", (unsigned long long)comparisons);
    std::printf("%llu exact packed-plus-converted block vertex comparisons; rigid and weighted groups, partial blocks, capacity/metadata failures and early cancellation\n", (unsigned long long)blockComparisons);
}
