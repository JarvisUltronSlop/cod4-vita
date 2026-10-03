#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

class VitaAssetRegistry;

struct ShipmentCollisionTrace {
    float fraction = 1.0f;
    float normal[3] = {0.0f, 0.0f, 1.0f};
    uint32_t contents = 0;
    bool startSolid = false;
    bool allSolid = false;
};

struct ShipmentCollisionSubmodel {
    float mins[3] = {};
    float maxs[3] = {};
    float radius = 0.0f;
};

// Pointer-free collision data used by the Vita movement bridge.  The reader
// intentionally accepts only the VITC sidecar emitted by the Windows oracle;
// native clipMap_t pointers never cross the package boundary.
class ShipmentCollision {
public:
    bool Load(const VitaAssetDatabase& database, uint64_t assetId = 0);
    void Clear();

    bool Loaded() const { return loaded_; }
    // VITC v3 is the first collision archive that preserves the exact
    // world/submodel brush membership and per-side surface flags required to
    // reconstruct genuine source cbrush_t/cmodel_t roots.  Older archives
    // remain readable by the scalar oracle, but cannot start the release
    // source session.
    bool SourceComplete() const { return loaded_ && wireVersion_ >= 3; }
    uint32_t WireVersion() const { return wireVersion_; }
    uint32_t PlaneCount() const { return static_cast<uint32_t>(planes_.size()); }
    uint32_t BrushCount() const { return static_cast<uint32_t>(brushes_.size()); }
    uint32_t VertexCount() const { return vertexCount_; }
    uint32_t TriangleCount() const { return triangleCount_; }
    const std::vector<ShipmentCollisionSubmodel>& Submodels() const {
        return submodels_;
    }
    bool WorldBounds(float mins[3], float maxs[3]) const;
    uint32_t BroadphaseCellCount() const {
        return static_cast<uint32_t>(brushGridCells_.size());
    }
    uint32_t BroadphaseReferenceCount() const {
        return static_cast<uint32_t>(brushGridBrushes_.size());
    }
    uint32_t BroadphaseGlobalBrushCount() const {
        return static_cast<uint32_t>(broadphaseGlobalBrushes_.size());
    }

    // Sweeps a sphere against the convex brush planes.  Pmove's capsule
    // bridge can use the same routine with the capsule radius; the brush
    // bounds and plane equations are already expanded without host pointers.
    bool TraceSphere(const float start[3], const float end[3], float radius,
                    uint32_t contentsMask, ShipmentCollisionTrace* result) const;

    // Tests a zero-radius sight ray against both the exported convex brushes
    // and the triangle soup carried by the VITC sidecar.  This complete query
    // remains available to gameplay/effect callers that need mesh collision;
    // source light visibility uses TraceLineBrushesOnly so an unpartitioned
    // triangle stream cannot become a global occluder.
    bool TraceLine(const float start[3], const float end[3],
                   uint32_t contentsMask, ShipmentCollisionTrace* result) const;

    // Matches the source CM_BoxSightTrace world path: sight queries use the
    // exported convex brush/BSP collision and do not treat the optional mesh
    // triangle soup as a global occluder.  The renderer uses this for sun
    // visibility so a terrain/patch triangle from another leaf cannot turn
    // every otherwise-open receiver into a shadowed surface.
    bool TraceLineBrushesOnly(const float start[3], const float end[3],
                              uint32_t contentsMask,
                              ShipmentCollisionTrace* result) const;

    // Sweeps the player bounds against the same pointer-free brush planes.
    // The support point is evaluated per plane, preserving the asymmetric
    // feet-to-head bounds used by COD4's pmove capsule bridge.
    bool TraceCapsule(const float start[3], const float mins[3],
                      const float maxs[3], const float end[3],
                      uint32_t contentsMask,
                      ShipmentCollisionTrace* result) const;

private:
    friend class VitaAssetRegistry;

    struct Plane {
        float normal[3];
        float dist;
        uint8_t type;
        uint8_t signbits;
    };
    struct BrushSide {
        Plane plane;
        uint32_t sourcePlane;
        uint32_t material;
        int32_t surfaceFlags;
    };
    struct Brush {
        float mins[3];
        float maxs[3];
        uint32_t contents;
        uint32_t sideOffset;
        uint32_t sideCount;
    };

    // The exported VITC brushes are source collision, not renderer geometry.
    // Keep a compact uniform-grid broadphase beside them so source light-grid
    // visibility and sun-shadow taps do not turn into an O(vertices * brushes)
    // walk on large authored maps.  The serialized VITC format is unchanged;
    // these are rebuildable runtime indices and never cross the backend ABI.
    struct BrushGridCell {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        uint32_t first = 0;
        uint32_t count = 0;
    };

    void BuildBrushBroadphase();

    std::vector<Plane> planes_;
    std::vector<Brush> brushes_;
    std::vector<BrushSide> sides_;
    std::vector<std::array<float, 3>> vertices_;
    std::vector<std::array<uint16_t, 3>> triangles_;
    std::vector<ShipmentCollisionSubmodel> submodels_;
    // One sorted brush-index list per source cmodel. Element zero is the
    // static world model; later elements preserve inline brush-model
    // membership for doors and authored map entities.
    std::vector<std::vector<uint16_t>> modelBrushes_;
    std::vector<BrushGridCell> brushGridCells_;
    std::vector<uint32_t> brushGridBrushes_;
    std::vector<uint32_t> broadphaseGlobalBrushes_;
    mutable std::vector<uint32_t> broadphaseVisitStamps_;
    mutable uint32_t broadphaseVisitStamp_ = 0;
    uint32_t vertexCount_ = 0;
    uint32_t triangleCount_ = 0;
    uint32_t wireVersion_ = 0;
    bool loaded_ = false;
};

} // namespace kisak::vita
