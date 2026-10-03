#pragma once

#include <array>
#include <cstdint>
#include <cmath>

#include <gfx/gfx_backend.h>

namespace kisak::vita {

// Immutable GPU-owned map stream. Handles are invalidated together when the
// map arena leaves; no desktop pointers or asset ownership cross this seam.
struct VitaMapRenderArchive {
    GfxBufferHandle vertexBuffer{0};
    GfxBufferHandle indexBuffer{0};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t surfaceCount = 0;
    uint64_t residentBytes = 0;
    bool ready = false;

    void Reset() { *this = {}; }
};

struct VitaDynamicEntityHandle {
    uint16_t entityNumber = 0;
    uint16_t renderAsset = 0;
    uint32_t flags = 0;
    float origin[3]{};
    float angles[3]{};
    float lightingOrigin[3]{};
    float materialTime = 0.0f;
    bool brushModel = false;
    bool rigidModel = false;
    float quaternion[4]{0, 0, 0, 1};
    float scale = 1;
};

struct VitaUiCommandHandle {
    uint16_t command = 0;
    uint16_t asset = 0;
};

// Per-frame simulation-to-render contract. The packet contains only copied
// view state and bounded IDs; referenced assets remain owned by the current
// map/frontend resource set.
struct VitaRenderPacket {
    static constexpr uint32_t kMaxDynamicEntities = 128;
    static constexpr uint32_t kMaxUiCommands = 192;

    float viewProjection[16]{};
    float cameraOrigin[3]{};
    float cameraAxis[3][3]{};
    float tanHalfFovX = 0.0f;
    float tanHalfFovY = 0.0f;
    float nearPlane = 2.0f;
    uint32_t mapIndex = UINT32_MAX;
    uint32_t mapGeneration = 0;
    uint32_t frameGeneration = 0;
    int32_t sourceServerTime = 0;
    uint32_t dynamicEntityCount = 0;
    uint32_t uiCommandCount = 0;
    uint32_t rejectedCommands = 0;
    uint32_t effectDrawCount = 0;
    uint32_t effectCloudCount = 0;
    uint32_t effectLightCount = 0;
    bool sourceViewValid = false;
    std::array<VitaDynamicEntityHandle, kMaxDynamicEntities> dynamicEntities{};
    std::array<VitaUiCommandHandle, kMaxUiCommands> uiCommands{};
};

// Convert the resolved source view to native OpenGL clip space. Source axis
// one points left; OpenGL X points right. D3D's infinite perspective depth
// 1-near/z becomes NDC 1-2*near/z before the viewport maps it back to [0,1].
inline bool VitaBuildViewProjection(VitaRenderPacket* packet) {
    if (!packet || !std::isfinite(packet->nearPlane) || packet->nearPlane <= 0
        || !std::isfinite(packet->tanHalfFovX) || packet->tanHalfFovX <= 0
        || !std::isfinite(packet->tanHalfFovY) || packet->tanHalfFovY <= 0)
        return false;
    float eyeRight = 0, eyeUp = 0, eyeForward = 0;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(packet->cameraOrigin[i])) return false;
        for (int j = 0; j < 3; ++j)
            if (!std::isfinite(packet->cameraAxis[j][i])) return false;
        const float right = -packet->cameraAxis[1][i];
        const float up = packet->cameraAxis[2][i];
        const float forward = packet->cameraAxis[0][i];
        packet->viewProjection[i * 4] = right / packet->tanHalfFovX;
        packet->viewProjection[i * 4 + 1] = up / packet->tanHalfFovY;
        packet->viewProjection[i * 4 + 2] = forward;
        packet->viewProjection[i * 4 + 3] = forward;
        eyeRight += right * packet->cameraOrigin[i];
        eyeUp += up * packet->cameraOrigin[i];
        eyeForward += forward * packet->cameraOrigin[i];
    }
    packet->viewProjection[12] = -eyeRight / packet->tanHalfFovX;
    packet->viewProjection[13] = -eyeUp / packet->tanHalfFovY;
    packet->viewProjection[14] = -eyeForward - 2 * packet->nearPlane;
    packet->viewProjection[15] = -eyeForward;
    return true;
}

struct VitaVisibilityReport {
    uint32_t sourceSurfaces = 0;
    uint32_t visibleSurfaces = 0;
    uint32_t submittedSurfaces = 0;
    uint32_t visibilityUsec = 0;
    uint32_t drawSubmitUsec = 0;
};

} // namespace kisak::vita
