#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_world_visibility.h>
#include <vita/vita_world_depth_order.h>
#include <vita/vita_render_contract.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string_view>

// Use the actual surface layout and draw function, without needing VitaSDK or
// game assets. The frozen reference uses the same camera and recording backend.
#include "world_visibility_types.inc"
using namespace kisak::vita;
struct ShipmentPreview {
    VitaMapRenderArchive nativeArchive{GfxBufferHandle{1}, GfxBufferHandle{2}, 0, 0, 0, 0, true};
    std::vector<ShipmentSurface> surfaces;
    std::vector<ShipmentSourceBounds> sourceSurfaceBounds;
    std::vector<uint32_t> visibleWorldSurfaces;
    VitaWorldDrawOrder worldDrawOrder;
    VitaDepthScratch worldDepthRanges;
};
namespace kisak::vita {
struct ShipmentDmPlayerState {};
VitaRenderPacket packet;
const VitaRenderPacket& VitaSourceRender_Packet() { return packet; }
uint64_t VitaCpuNow() { return 0; }
void VitaCpuProfile_Counter(const char*, uint64_t, uint64_t = 0) {}
template<class T> bool VitaCpuRender_Copy(const T& value, void (*execute)(const T&)) {
    execute(value);
    return true;
}
}
#define VITA_CPU_SCOPE(name)
uint64_t sceKernelGetProcessTimeWide() { return 0; }
NativeViewFrustum currentView;
NativeViewFrustum BuildNativeViewFrustum(const ShipmentDmPlayerState&, float) { return currentView; }

bool recording = true;
std::vector<uint32_t> commands;
uint32_t submittedHash = 0;
void Record(std::initializer_list<uint32_t> values) {
    if (recording) commands.insert(commands.end(), values);
}
void RecordFloats(const float* values, unsigned count) {
    if (!recording) return;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t bits;
        std::memcpy(&bits, values + i, sizeof(bits));
        commands.push_back(bits);
    }
}
void GfxVita_SetDrawSurfaceIndex(uint32_t index, bool constant) { Record({1, index, constant}); }
GfxBackendApi Backend() {
    GfxBackendApi gfx{};
    gfx.SetWorldViewProjection = +[](const float* matrix, float near) {
        Record({2}); RecordFloats(matrix, 16); RecordFloats(&near, 1);
    };
    gfx.SetSurfaceMode = +[](GfxSurfaceMode mode) { Record({3, uint32_t(mode)}); };
    gfx.SetAlphaTestFunc = +[](GfxAlphaTestFunc mode) { Record({4, uint32_t(mode)}); };
    gfx.SetCullMode = +[](GfxCullMode mode) { Record({5, uint32_t(mode)}); };
    gfx.BindTexture = +[](GfxTextureHandle texture) { Record({6, texture.value}); };
    gfx.SetSamplerState = +[](uint32_t unit, uint8_t state) { Record({7, unit, state}); };
    gfx.SetMaterialPass = +[](const GfxMaterialPassState* p) {
        Record({8, p->colorTexture.value, p->normalTexture.value, p->specularTexture.value,
            p->reflectionProbeTexture.value, p->lightmapPrimaryTexture.value,
            p->lightmapSecondaryTexture.value, p->colorSamplerState, p->normalSamplerState,
            p->specularSamplerState, p->lightmapSamplerFlags, p->techniqueFlags0, p->techniqueFlags1,
            uint32_t(p->surfaceMode), uint32_t(p->alphaTestFunc), uint32_t(p->cullMode),
            p->sourceSurfaceIndex, p->sourceStateBits0, p->sourceStateBits1, p->sourceStateValid});
    };
    gfx.SetDrawLightBounds = +[](const GfxDrawLightBounds* b) {
        Record({9, b->valid}); RecordFloats(b->minimum, 3); RecordFloats(b->maximum, 3);
    };
    gfx.DrawStaticIndexed = +[](GfxBufferHandle vb, uint32_t first, uint32_t vertices,
                                GfxBufferHandle ib, uint32_t start, uint32_t indices) {
        Record({10, vb.value, first, vertices, ib.value, start, indices});
        submittedHash = submittedHash * 33u + first + start + indices;
        return true;
    };
    return gfx;
}
#include "world_visibility_draw.inc"
#include "world_visibility_reference.inc"

void SetView(float angle, float scale = 1.0f) {
    currentView = {};
    currentView.eye[0] = std::sin(angle * 0.7f) * 96;
    currentView.eye[1] = std::cos(angle * 0.3f) * 96;
    currentView.eye[2] = std::sin(angle * 0.2f) * 40;
    const float pitch = std::sin(angle) * 0.5f;
    currentView.forward[0] = std::cos(angle) * std::cos(pitch);
    currentView.forward[1] = std::sin(angle) * std::cos(pitch);
    currentView.forward[2] = std::sin(pitch);
    currentView.right[0] = std::sin(angle);
    currentView.right[1] = -std::cos(angle);
    currentView.up[0] = -std::cos(angle) * std::sin(pitch);
    currentView.up[1] = -std::sin(angle) * std::sin(pitch);
    currentView.up[2] = std::cos(pitch);
    currentView.eyeForward = currentView.forward[0] * currentView.eye[0]
        + currentView.forward[1] * currentView.eye[1] + currentView.forward[2] * currentView.eye[2];
    currentView.horizontalScale = scale;
    currentView.verticalScale = scale * 1.6f;
    packet = {};
    packet.sourceViewValid = true;
    std::copy_n(currentView.eye, 3, packet.cameraOrigin);
    std::copy_n(currentView.forward, 3, packet.cameraAxis[0]);
    std::copy_n(currentView.up, 3, packet.cameraAxis[2]);
    for (unsigned a = 0; a < 3; ++a) packet.cameraAxis[1][a] = -currentView.right[a];
    packet.tanHalfFovX = 1 / currentView.horizontalScale;
    packet.tanHalfFovY = 1 / currentView.verticalScale;
    packet.nearPlane = currentView.nearDistance;
    assert(VitaBuildViewProjection(&packet));
}
float RandomFloat(std::mt19937& rng, float magnitude) {
    return (int(rng() % 20001) - 10000) * (magnitude / 10000.0f);
}
void Fixture(ShipmentPreview& p, unsigned count, unsigned seed, bool grouped = false) {
    std::mt19937 rng(seed);
    p.worldDrawOrder.Clear();
    p.surfaces.assign(count, {});
    p.sourceSurfaceBounds.assign(count, {});
    p.visibleWorldSurfaces.reserve(count);
    assert(p.worldDepthRanges.Reserve(count));
    p.nativeArchive.surfaceCount = count;
    for (unsigned i = 0; i < count; ++i) {
        auto& s = p.surfaces[i];
        s.vertexCount = 3 + rng() % 120;
        s.triangleCount = 1 + rng() % 40;
        s.firstVertex = i * 128;
        s.baseIndex = i * 256;
        s.mode = GfxSurfaceMode(rng() % 7);
        s.sourceTechniqueFlags0 = rng() % 4;
        s.sourceStateBits0 = 0x18000800u | (rng() % 4);
        s.sourceStateBits1 = rng() % 2 ? 5u : 13u;
        s.sourceStateValid = 1;
        s.texture.value = 1 + rng() % 24;
        s.normalTexture.value = rng() % 4;
        s.specularTexture.value = rng() % 4;
        s.lightmapTexture.value = 1 + rng() % 3;
        s.secondaryLightmapTexture.value = rng() % 3;
        s.constantWorldBasis = rng() % 2;
        if (i % 37 == 0) s.cameraRegion = 3;
        if (i % 41 == 0) s.vertexCount = 0;
        if (i % 43 == 0) s.triangleCount = 0;
        auto& b = p.sourceSurfaceBounds[i];
        b.valid = i % 53 != 0;
        for (unsigned a = 0; a < 3; ++a) {
            const float center = RandomFloat(rng, a == 2 ? 200.0f : 800.0f);
            const float extent = 8 + rng() % 128;
            b.minimum[a] = center - extent;
            b.maximum[a] = center + extent;
        }
    }
    // Give the baseline an already sorted archive too. Sort by every legacy
    // key, keeping each surface paired with its original spatial bounds.
    if (grouped) {
        std::vector<uint32_t> order(count);
        for (unsigned i = 0; i < count; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return VitaWorldSurfaceLess(p.surfaces.data(), a, b);
        });
        const auto surfaces = p.surfaces;
        const auto bounds = p.sourceSurfaceBounds;
        for (unsigned i = 0; i < count; ++i) {
            p.surfaces[i] = surfaces[order[i]];
            p.sourceSurfaceBounds[i] = bounds[order[i]];
        }
    }
}

uint64_t comparedFrames = 0, comparedBounds = 0;
void Compare(ShipmentPreview& p, uint32_t depthPhase = 0) {
    const auto gfx = Backend();
    VitaVisibilityReport before{}, after{};
    commands.clear();
    const bool oldResult = ReferenceDrawNativeWorld(&gfx, &p, {}, 65, &before, depthPhase);
    const auto oldCommands = commands;
    const auto oldVisible = p.visibleWorldSurfaces;
    commands.clear();
    const bool newResult = DrawNativeWorld(&gfx, &p, {}, 65, &after, depthPhase);
    assert(oldResult == newResult && commands == oldCommands && p.visibleWorldSurfaces == oldVisible);
    assert(before.sourceSurfaces == after.sourceSurfaces && before.visibleSurfaces == after.visibleSurfaces
        && before.submittedSurfaces == after.submittedSurfaces);
    ++comparedFrames;
}

void Boundaries() {
    SetView(0);
    currentView.eye[0] = currentView.eye[1] = currentView.eye[2] = currentView.eyeForward = 0;
    currentView.horizontalScale = currentView.verticalScale = 1;
    currentView.nearDistance = 2;
    currentView.farDistance = 100;
    const VitaWorldFrustum frustum(currentView);
    // Just inside, exactly touching and just outside every clipping plane.
    for (unsigned plane = 0; plane < 6; ++plane) {
        for (float delta : {-0.001f, 0.0f, 0.001f}) {
            ShipmentSourceBounds b;
            b.valid = true;
            b.minimum[0] = b.maximum[0] = plane == 0 ? 2 + delta : plane == 1 ? 100 + delta : 20;
            b.minimum[1] = b.maximum[1] = plane == 2 ? -20 + delta : plane == 3 ? 20 + delta : 0;
            b.minimum[2] = b.maximum[2] = plane == 4 ? -20 + delta : plane == 5 ? 20 + delta : 0;
            assert(frustum.Visible(b) == ReferenceBoundsVisible(currentView, b));
            if (delta == 0) assert(frustum.Visible(b));
            ++comparedBounds;
        }
    }
    std::mt19937 rng(19);
    for (unsigned frame = 0; frame < 1000; ++frame) {
        SetView(RandomFloat(rng, 20), 0.1f + (rng() % 100) * 0.1f);
        const VitaWorldFrustum moving(currentView);
        for (unsigned i = 0; i < 1000; ++i) {
            ShipmentSourceBounds b;
            b.valid = i % 31 != 0;
            for (unsigned a = 0; a < 3; ++a) {
                b.minimum[a] = RandomFloat(rng, 32768);
                b.maximum[a] = b.minimum[a] + rng() % 512;
            }
            if (i % 97 == 0) b.minimum[1] = std::numeric_limits<float>::quiet_NaN();
            if (i % 101 == 0) b.maximum[2] = std::numeric_limits<float>::infinity();
            assert(moving.Visible(b) == ReferenceBoundsVisible(currentView, b));
            ++comparedBounds;
        }
    }
}

unsigned failedAllocations = 0;
uint32_t* FailAllocation(size_t) { ++failedAllocations; return nullptr; }
void Parity() {
    ShipmentPreview p;
    for (unsigned map = 0; map < 60; ++map) {
        Fixture(p, map == 0 ? 0 : map == 1 ? 1 : 128 + map * 17, 91 + map);
        for (unsigned frame = 0; frame < 40; ++frame) {
            SetView(frame * 0.17f, 0.4f + (frame % 11) * 0.3f);
            if (!p.surfaces.empty() && frame % 8 == 0) {
                auto& s = p.surfaces[(frame * 13) % p.surfaces.size()];
                s.texture.value ^= 0x100u;
                s.lightmapTexture.value ^= 0x400u;
                s.secondaryLightmapTexture.value ^= 0x200u;
                s.mode = GfxSurfaceMode((uint32_t(s.mode) + 1) % 7);
                s.sourceTechniqueFlags0 ^= 7;
                s.sourceStateBits0 ^= 0x100u;
                s.sourceStateBits1 ^= 3;
                p.worldDrawOrder.Invalidate();
            }
            Compare(p, frame % 3);
        }
    }
    Fixture(p, 256, 42);
    SetView(0);
    // Equal keys retain source order even for overlapping translucent draws.
    for (auto& s : p.surfaces) {
        s.mode = GfxSurfaceMode::AlphaBlend;
        s.sourceTechniqueFlags0 = s.sourceStateBits0 = s.sourceStateBits1 = 0;
        s.texture.value = s.lightmapTexture.value = s.secondaryLightmapTexture.value = 1;
    }
    p.sourceSurfaceBounds.clear(); // Missing bounds must stay visible.
    Compare(p);
    assert(std::is_sorted(p.visibleWorldSurfaces.begin(), p.visibleWorldSurfaces.end()));
    // Eligibility/bounds can change without affecting cached material order.
    p.surfaces[4].cameraRegion = 3;
    p.surfaces[5].triangleCount = 0;
    Compare(p);
    p.sourceSurfaceBounds.resize(p.surfaces.size());
    auto& changedBounds = p.sourceSurfaceBounds[6];
    changedBounds.valid = true;
    std::fill_n(changedBounds.minimum, 3, -50000.0f);
    std::fill_n(changedBounds.maximum, 3, -49000.0f);
    Compare(p);
    changedBounds.valid = false;
    Compare(p);
    p.worldDrawOrder.Clear();
    assert(!p.worldDrawOrder.Prepare(p.surfaces.data(), p.surfaces.size(), FailAllocation));
    for (unsigned i = 0; i < 8; ++i) Compare(p);
    assert(failedAllocations == 1 && p.worldDrawOrder.Bytes() == 0);
    p.worldDrawOrder.Invalidate();
    assert(p.worldDrawOrder.Prepare(p.surfaces.data(), p.surfaces.size()));
    assert(p.worldDrawOrder.Bytes() == p.surfaces.size() * sizeof(uint32_t));
    Compare(p);
    // Growing a map cannot expose the smaller previous allocation on failure.
    p.surfaces.resize(512, p.surfaces.front());
    assert(!p.worldDrawOrder.Prepare(p.surfaces.data(), p.surfaces.size(), FailAllocation));
    Compare(p);
    p.surfaces.resize(32);
    Compare(p);
    p.worldDrawOrder.Clear();
    assert(p.worldDrawOrder.Bytes() == 0);
    assert(!p.worldDrawOrder.Prepare(p.surfaces.data(), SIZE_MAX, FailAllocation));
    Compare(p);
    packet.sourceViewValid = false;
    Compare(p);
    Boundaries();
    std::printf("world visibility: %llu identical draw streams, %llu identical bounds decisions\n",
        static_cast<unsigned long long>(comparedFrames), static_cast<unsigned long long>(comparedBounds));
}

volatile uint32_t benchmarkSink = 0;
void Benchmark() {
    recording = false;
    const auto gfx = Backend();
    using Clock = std::chrono::steady_clock;
    std::printf("synthetic host world preparation + stub submission; not Vita frame time\n");
    std::printf("surfaces,layout,view,visible_mean,cache_bytes,build_us,reference_us,candidate_us,speedup\n");
    for (unsigned count : {256u, 1024u, 4096u}) for (bool grouped : {true, false})
      for (float scale : {0.7f, 3.0f}) {
        ShipmentPreview p;
        Fixture(p, count, 73, grouped);
        const auto buildStart = Clock::now();
        assert(p.worldDrawOrder.Prepare(p.surfaces.data(), p.surfaces.size()));
        const double buildUs = std::chrono::duration<double, std::micro>(Clock::now() - buildStart).count();
        std::array<double, 9> oldTimes{}, newTimes{};
        uint64_t visible = 0;
        const auto run = [&](bool candidate, bool countVisible) {
            const auto start = Clock::now();
            for (unsigned frame = 0; frame < 240; ++frame) {
                SetView(frame * 0.013f, scale);
                if (candidate) DrawNativeWorld(&gfx, &p, {}, 65);
                else ReferenceDrawNativeWorld(&gfx, &p, {}, 65);
                if (countVisible) visible += p.visibleWorldSurfaces.size();
            }
            benchmarkSink = submittedHash;
            return std::chrono::duration<double, std::micro>(Clock::now() - start).count() / 240;
        };
        run(false, false); run(true, true);
        for (unsigned sample = 0; sample < oldTimes.size(); ++sample) {
            // Alternate order to avoid consistently favoring the warmest run.
            if (sample % 2) { newTimes[sample] = run(true, false); oldTimes[sample] = run(false, false); }
            else { oldTimes[sample] = run(false, false); newTimes[sample] = run(true, false); }
        }
        std::sort(oldTimes.begin(), oldTimes.end());
        std::sort(newTimes.begin(), newTimes.end());
        std::printf("%u,%s,%s,%.1f,%zu,%.2f,%.2f,%.2f,%.2fx\n", count,
            grouped ? "presorted" : "shuffled", scale < 1 ? "wide" : "narrow",
            visible / 240.0, p.worldDrawOrder.Bytes(), buildUs, oldTimes[4], newTimes[4], oldTimes[4] / newTimes[4]);
    }
}
int main(int argc, char** argv) {
    commands.reserve(200000);
    Parity();
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") Benchmark();
}
