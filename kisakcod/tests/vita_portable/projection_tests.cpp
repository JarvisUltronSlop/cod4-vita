#include <gfx_d3d/r_scene_clip.h>
#include <vita/vita_render_contract.h>
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    using namespace kisak::vita;
    VitaRenderPacket packet{};
    // A translated camera facing +Y exercises all three position axes.
    packet.cameraOrigin[0] = 164; packet.cameraOrigin[1] = -591;
    packet.cameraOrigin[2] = 253;
    packet.cameraAxis[0][1] = 1; packet.cameraAxis[1][0] = -1;
    packet.cameraAxis[2][2] = 1;
    packet.tanHalfFovX = 0.843; packet.tanHalfFovY = 0.478;
    const auto WindowDepth = [&](float distance) {
        const float point[4]{164, -591 + distance, 253, 1};
        float clipZ = 0, clipW = 0;
        for (int i = 0; i < 4; ++i) {
            clipZ += packet.viewProjection[i * 4 + 2] * point[i];
            clipW += packet.viewProjection[i * 4 + 3] * point[i];
        }
        return (clipZ / clipW + 1) * 0.5f;
    };
    // Reproduce the cgame zero sentinel. It must produce ordinary occlusion
    // at different ranges, not put every surface at the far depth value.
    packet.nearPlane = R_ResolveNearClip(0, R_DEFAULT_NEAR_CLIP);
    Require(packet.nearPlane == 4 && VitaBuildViewProjection(&packet));
    Require(std::abs(WindowDepth(4)) < 0.00001f);
    Require(std::abs(WindowDepth(8) - 0.5f) < 0.00001f);
    Require(WindowDepth(8) < WindowDepth(64) && WindowDepth(64) < WindowDepth(4096));
    Require(WindowDepth(2) < 0); // inside the source near clip
    packet.nearPlane = R_ResolveNearClip(0.125f, 16);
    Require(packet.nearPlane == 0.125f && VitaBuildViewProjection(&packet));
    Require(std::abs(WindowDepth(0.25f) - 0.5f) < 0.00001f);
    Require(R_ResolveNearClip(-1, 8) == 8);
    Require(R_ResolveNearClip(0, 0.001f) == 0.01f);
    packet.nearPlane = 0;
    Require(!VitaBuildViewProjection(&packet));
    packet.nearPlane = std::numeric_limits<float>::quiet_NaN();
    Require(!VitaBuildViewProjection(&packet));
    std::puts("source near-clip resolution and native depth separation passed");
}
