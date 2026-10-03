#include <vita/vita_shipment_collision.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x43544956; // "VITC"
constexpr uint32_t kVersionLegacy = 2;
constexpr uint32_t kVersionSourceComplete = 3;
constexpr uint32_t kMaxCount = 2'000'000;
constexpr float kBrushGridCellSize = 256.0f;
constexpr uint32_t kBrushGridMaxCellsPerBrush = 4096u;
constexpr uint32_t kBrushGridMaxReferences = 1'000'000u;
constexpr uint64_t kBrushGridMaxQueryCells = 4096u;

uint32_t ReadU32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

uint16_t ReadU16(const uint8_t* data) {
    uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

float ReadFloat(const uint8_t* data) {
    float value = 0.0f;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool IsRangeValid(uint32_t offset, uint32_t count, uint32_t total) {
    return offset <= total && count <= total - offset;
}

float Dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool CellBefore(int32_t ax, int32_t ay, int32_t az,
                int32_t bx, int32_t by, int32_t bz) {
    if (ax != bx)
        return ax < bx;
    if (ay != by)
        return ay < by;
    return az < bz;
}

bool PositionToCell(float value, int32_t* output) {
    if (!output || !std::isfinite(value))
        return false;
    const double coordinate = std::floor(
        static_cast<double>(value) / static_cast<double>(kBrushGridCellSize));
    if (coordinate < static_cast<double>(std::numeric_limits<int32_t>::min())
        || coordinate > static_cast<double>(std::numeric_limits<int32_t>::max()))
        return false;
    *output = static_cast<int32_t>(coordinate);
    return true;
}

bool ProductWithinLimit(uint64_t first, uint64_t second, uint64_t third,
                        uint64_t limit, uint64_t* output) {
    if (!output || first == 0u || second == 0u || third == 0u) {
        if (output)
            *output = 0u;
        return first == 0u || second == 0u || third == 0u;
    }
    if (first > limit / second)
        return false;
    const uint64_t partial = first * second;
    if (partial > limit / third)
        return false;
    *output = partial * third;
    return true;
}

} // namespace

void ShipmentCollision::Clear() {
    planes_.clear();
    brushes_.clear();
    sides_.clear();
    vertices_.clear();
    triangles_.clear();
    submodels_.clear();
    modelBrushes_.clear();
    brushGridCells_.clear();
    brushGridBrushes_.clear();
    broadphaseGlobalBrushes_.clear();
    broadphaseVisitStamps_.clear();
    broadphaseVisitStamp_ = 0;
    vertexCount_ = 0;
    triangleCount_ = 0;
    wireVersion_ = 0;
    loaded_ = false;
}

void ShipmentCollision::BuildBrushBroadphase() {
    brushGridCells_.clear();
    brushGridBrushes_.clear();
    broadphaseGlobalBrushes_.clear();
    broadphaseVisitStamps_.clear();
    broadphaseVisitStamp_ = 0;
    if (brushes_.empty())
        return;

    struct BrushGridReference {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        uint32_t brush = 0;
    };
    std::vector<BrushGridReference> references;
    references.reserve(std::min<size_t>(brushes_.size() * 8u,
                                        kBrushGridMaxReferences));

    for (uint32_t brushIndex = 0;
         brushIndex < static_cast<uint32_t>(brushes_.size()); ++brushIndex) {
        const Brush& brush = brushes_[brushIndex];
        if (brush.sideCount == 0)
            continue;

        int32_t minimum[3]{};
        int32_t maximum[3]{};
        bool validBounds = true;
        for (uint32_t axis = 0; axis < 3u; ++axis) {
            if (!PositionToCell(brush.mins[axis], &minimum[axis])
                || !PositionToCell(brush.maxs[axis], &maximum[axis])
                || maximum[axis] < minimum[axis]) {
                validBounds = false;
                break;
            }
        }
        if (!validBounds) {
            broadphaseGlobalBrushes_.push_back(brushIndex);
            continue;
        }

        const uint64_t spanX = static_cast<uint64_t>(
            static_cast<int64_t>(maximum[0]) - minimum[0]) + 1u;
        const uint64_t spanY = static_cast<uint64_t>(
            static_cast<int64_t>(maximum[1]) - minimum[1]) + 1u;
        const uint64_t spanZ = static_cast<uint64_t>(
            static_cast<int64_t>(maximum[2]) - minimum[2]) + 1u;
        uint64_t cellCount = 0u;
        if (!ProductWithinLimit(spanX, spanY, spanZ,
                                kBrushGridMaxCellsPerBrush, &cellCount)
            || references.size() > kBrushGridMaxReferences
            || cellCount > kBrushGridMaxReferences
            || references.size() + static_cast<size_t>(cellCount)
                > kBrushGridMaxReferences) {
            // Very large world brushes are cheaper and safer to test through
            // a small global list than to expand them into thousands of
            // duplicate grid references.  A reference overflow disables the
            // grid entirely; the query code then retains the original full
            // brush walk instead of accepting an incomplete broadphase.
            if (cellCount > kBrushGridMaxCellsPerBrush) {
                broadphaseGlobalBrushes_.push_back(brushIndex);
                continue;
            }
            brushGridCells_.clear();
            brushGridBrushes_.clear();
            broadphaseGlobalBrushes_.clear();
            broadphaseVisitStamps_.clear();
            return;
        }

        for (int64_t x = minimum[0]; x <= maximum[0]; ++x) {
            for (int64_t y = minimum[1]; y <= maximum[1]; ++y) {
                for (int64_t z = minimum[2]; z <= maximum[2]; ++z)
                    references.push_back({static_cast<int32_t>(x),
                                          static_cast<int32_t>(y),
                                          static_cast<int32_t>(z), brushIndex});
            }
        }
    }

    std::sort(references.begin(), references.end(),
              [](const BrushGridReference& left,
                 const BrushGridReference& right) {
                  if (CellBefore(left.x, left.y, left.z,
                                 right.x, right.y, right.z))
                      return true;
                  if (CellBefore(right.x, right.y, right.z,
                                 left.x, left.y, left.z))
                      return false;
                  return left.brush < right.brush;
              });
    brushGridCells_.reserve(references.size());
    brushGridBrushes_.reserve(references.size());
    for (const BrushGridReference& reference : references) {
        if (brushGridCells_.empty()
            || CellBefore(brushGridCells_.back().x,
                          brushGridCells_.back().y,
                          brushGridCells_.back().z,
                          reference.x, reference.y, reference.z)
            || CellBefore(reference.x, reference.y, reference.z,
                          brushGridCells_.back().x,
                          brushGridCells_.back().y,
                          brushGridCells_.back().z)) {
            brushGridCells_.push_back({reference.x, reference.y, reference.z,
                                       static_cast<uint32_t>(
                                           brushGridBrushes_.size()), 0u});
        }
        brushGridBrushes_.push_back(reference.brush);
        ++brushGridCells_.back().count;
    }
    broadphaseVisitStamps_.assign(brushes_.size(), 0u);
}

bool ShipmentCollision::Load(const VitaAssetDatabase& database, uint64_t assetId) {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::Collision, assetId) : nullptr;
    if (assetId && !requested)
        return false;
    for (uint32_t chunkIndex = 0; chunkIndex < database.ChunkCount(); ++chunkIndex) {
        const KvapChunk* chunk = database.Chunk(chunkIndex);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Collision)
            || (assetId && chunk != requested)
            || chunk->rawSize < 32 || chunk->rawSize > 32 * 1024 * 1024)
            continue;

        std::vector<uint8_t> bytes(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, bytes.data(), bytes.size()) != KvapError::Ok)
            continue;
        const uint32_t wireVersion = ReadU32(bytes.data() + 4);
        if (ReadU32(bytes.data()) != kMagic
            || (wireVersion != kVersionLegacy
                && wireVersion != kVersionSourceComplete))
            continue;

        const uint32_t planeCount = ReadU32(bytes.data() + 8);
        const uint32_t brushCount = ReadU32(bytes.data() + 12);
        const uint32_t sideCount = ReadU32(bytes.data() + 16);
        const uint32_t vertexCount = ReadU32(bytes.data() + 20);
        const uint32_t triangleCount = ReadU32(bytes.data() + 24);
        const uint32_t submodelCount = ReadU32(bytes.data() + 28);
        if (planeCount > kMaxCount || brushCount > 65535 || sideCount > kMaxCount
            || vertexCount > kMaxCount || triangleCount > kMaxCount || submodelCount > 65535)
            continue;
        const uint64_t legacyBytes = 32ull
            + static_cast<uint64_t>(planeCount) * 18ull
            + static_cast<uint64_t>(brushCount) * 36ull
            + static_cast<uint64_t>(sideCount) * 24ull
            + static_cast<uint64_t>(vertexCount) * 12ull
            + static_cast<uint64_t>(triangleCount) * 6ull
            + static_cast<uint64_t>(submodelCount) * 28ull;
        uint32_t membershipCount = 0;
        uint64_t expected = legacyBytes;
        if (wireVersion == kVersionSourceComplete) {
            const uint64_t tailHeader = legacyBytes
                + static_cast<uint64_t>(sideCount) * sizeof(int32_t);
            if (tailHeader + 8ull
                    + static_cast<uint64_t>(submodelCount + 1u)
                        * sizeof(uint32_t) > bytes.size())
                continue;
            const uint8_t* tail = bytes.data()
                + static_cast<size_t>(tailHeader);
            const uint32_t modelCount = ReadU32(tail);
            membershipCount = ReadU32(tail + 4);
            if (modelCount != submodelCount || membershipCount > kMaxCount)
                continue;
            expected = tailHeader + 8ull
                + static_cast<uint64_t>(submodelCount + 1u)
                    * sizeof(uint32_t)
                + static_cast<uint64_t>(membershipCount) * sizeof(uint16_t);
        }
        if (expected != bytes.size())
            continue;

        planes_.resize(planeCount);
        const uint8_t* cursor = bytes.data() + 32;
        for (uint32_t i = 0; i < planeCount; ++i) {
            Plane& plane = planes_[i];
            plane.normal[0] = ReadFloat(cursor);
            plane.normal[1] = ReadFloat(cursor + 4);
            plane.normal[2] = ReadFloat(cursor + 8);
            plane.dist = ReadFloat(cursor + 12);
            plane.type = cursor[16];
            plane.signbits = cursor[17];
            cursor += 18;
        }

        brushes_.resize(brushCount);
        for (uint32_t i = 0; i < brushCount; ++i) {
            Brush& brush = brushes_[i];
            brush.mins[0] = ReadFloat(cursor);
            brush.mins[1] = ReadFloat(cursor + 4);
            brush.mins[2] = ReadFloat(cursor + 8);
            brush.contents = ReadU32(cursor + 12);
            brush.maxs[0] = ReadFloat(cursor + 16);
            brush.maxs[1] = ReadFloat(cursor + 20);
            brush.maxs[2] = ReadFloat(cursor + 24);
            brush.sideOffset = ReadU32(cursor + 28);
            brush.sideCount = ReadU32(cursor + 32);
            cursor += 36;
            if (!IsRangeValid(brush.sideOffset, brush.sideCount, sideCount)) {
                Clear();
                break;
            }
        }
        if (brushes_.size() != brushCount)
            continue;

        sides_.resize(sideCount);
        for (uint32_t i = 0; i < sideCount; ++i) {
            BrushSide& side = sides_[i];
            side.sourcePlane = ReadU32(cursor);
            side.plane.normal[0] = ReadFloat(cursor + 4);
            side.plane.normal[1] = ReadFloat(cursor + 8);
            side.plane.normal[2] = ReadFloat(cursor + 12);
            side.plane.dist = ReadFloat(cursor + 16);
            side.material = ReadU32(cursor + 20);
            side.surfaceFlags = 0;
            cursor += 24;
        }
        vertices_.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i) {
            vertices_[i][0] = ReadFloat(cursor);
            vertices_[i][1] = ReadFloat(cursor + 4);
            vertices_[i][2] = ReadFloat(cursor + 8);
            cursor += 12;
        }
        triangles_.resize(triangleCount);
        bool invalidTriangle = false;
        for (uint32_t i = 0; i < triangleCount; ++i) {
            const uint16_t a = ReadU16(cursor);
            const uint16_t b = ReadU16(cursor + 2);
            const uint16_t c = ReadU16(cursor + 4);
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount) {
                invalidTriangle = true;
                break;
            }
            triangles_[i] = {a, b, c};
            cursor += 6;
        }
        if (invalidTriangle)
        {
            Clear();
            continue;
        }
        submodels_.resize(submodelCount);
        for (uint32_t i = 0; i < submodelCount; ++i) {
            ShipmentCollisionSubmodel& submodel = submodels_[i];
            for (uint32_t axis = 0; axis < 3; ++axis)
                submodel.mins[axis] = ReadFloat(cursor + axis * 4u);
            for (uint32_t axis = 0; axis < 3; ++axis)
                submodel.maxs[axis] = ReadFloat(cursor + 12u + axis * 4u);
            submodel.radius = ReadFloat(cursor + 24u);
            cursor += 28u;
        }
        modelBrushes_.assign(submodelCount, {});
        if (wireVersion == kVersionSourceComplete) {
            for (uint32_t i = 0; i < sideCount; ++i) {
                sides_[i].surfaceFlags = static_cast<int32_t>(ReadU32(cursor));
                cursor += sizeof(int32_t);
            }
            const uint32_t modelCount = ReadU32(cursor);
            const uint32_t serializedMembershipCount = ReadU32(cursor + 4);
            cursor += 8;
            if (modelCount != submodelCount
                || serializedMembershipCount != membershipCount) {
                Clear();
                continue;
            }
            std::vector<uint32_t> offsets(submodelCount + 1u);
            bool validOffsets = true;
            for (uint32_t i = 0; i <= submodelCount; ++i) {
                offsets[i] = ReadU32(cursor);
                cursor += sizeof(uint32_t);
                if (offsets[i] > membershipCount
                    || (i && offsets[i] < offsets[i - 1u]))
                    validOffsets = false;
            }
            if (!validOffsets || offsets.front() != 0u
                || offsets.back() != membershipCount) {
                Clear();
                continue;
            }
            for (uint32_t modelIndex = 0; modelIndex < submodelCount;
                 ++modelIndex) {
                auto& membership = modelBrushes_[modelIndex];
                membership.reserve(offsets[modelIndex + 1u]
                                   - offsets[modelIndex]);
                uint16_t previous = 0;
                bool first = true;
                for (uint32_t item = offsets[modelIndex];
                     item < offsets[modelIndex + 1u]; ++item) {
                    const uint16_t brush = ReadU16(
                        cursor + static_cast<size_t>(item) * sizeof(uint16_t));
                    if (brush >= brushCount
                        || (!first && brush <= previous)) {
                        validOffsets = false;
                        break;
                    }
                    membership.push_back(brush);
                    previous = brush;
                    first = false;
                }
                if (!validOffsets)
                    break;
            }
            if (!validOffsets) {
                Clear();
                continue;
            }
            cursor += static_cast<size_t>(membershipCount) * sizeof(uint16_t);
        } else if (!modelBrushes_.empty()) {
            // V2 did not preserve brush-model ownership.  Retain its historic
            // scalar-oracle behavior by treating every brush as world-owned;
            // SourceComplete() remains false so this approximation can never
            // enter the release source server.
            auto& worldBrushes = modelBrushes_.front();
            worldBrushes.reserve(brushCount);
            for (uint32_t brush = 0; brush < brushCount; ++brush)
                worldBrushes.push_back(static_cast<uint16_t>(brush));
        }
        if (cursor != bytes.data() + bytes.size()) {
            Clear();
            continue;
        }
        vertexCount_ = vertexCount;
        triangleCount_ = triangleCount;
        BuildBrushBroadphase();
        wireVersion_ = wireVersion;
        loaded_ = true;
        return true;
    }
    return false;
}

bool ShipmentCollision::WorldBounds(float mins[3], float maxs[3]) const {
    if (!loaded_ || !mins || !maxs)
        return false;
    if (!submodels_.empty()) {
        std::memcpy(mins, submodels_.front().mins, sizeof(float) * 3u);
        std::memcpy(maxs, submodels_.front().maxs, sizeof(float) * 3u);
        return true;
    }
    if (brushes_.empty())
        return false;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        mins[axis] = brushes_.front().mins[axis];
        maxs[axis] = brushes_.front().maxs[axis];
    }
    for (const Brush& brush : brushes_) {
        for (uint32_t axis = 0; axis < 3; ++axis) {
            mins[axis] = std::min(mins[axis], brush.mins[axis]);
            maxs[axis] = std::max(maxs[axis], brush.maxs[axis]);
        }
    }
    return true;
}

bool ShipmentCollision::TraceSphere(const float start[3], const float end[3], float radius,
                                    uint32_t contentsMask,
                                    ShipmentCollisionTrace* result) const {
    if (!loaded_ || !start || !end || !result)
        return false;
    *result = ShipmentCollisionTrace{};
    result->fraction = 1.0f;
    const float safeRadius = std::max(radius, 0.0f);
    bool hit = false;
    const auto TestBrush = [&](uint32_t brushIndex) {
        if (brushIndex >= brushes_.size())
            return;
        if (!broadphaseVisitStamps_.empty()) {
            if (brushIndex >= broadphaseVisitStamps_.size())
                return;
            if (broadphaseVisitStamps_[brushIndex] == broadphaseVisitStamp_)
                return;
            broadphaseVisitStamps_[brushIndex] = broadphaseVisitStamp_;
        }
        const Brush& brush = brushes_[brushIndex];
        if (brush.sideCount == 0)
            return;
        if (contentsMask && !(brush.contents & contentsMask))
            return;
        if (start[0] < brush.mins[0] - safeRadius && end[0] < brush.mins[0] - safeRadius)
            return;
        if (start[0] > brush.maxs[0] + safeRadius && end[0] > brush.maxs[0] + safeRadius)
            return;
        if (start[1] < brush.mins[1] - safeRadius && end[1] < brush.mins[1] - safeRadius)
            return;
        if (start[1] > brush.maxs[1] + safeRadius && end[1] > brush.maxs[1] + safeRadius)
            return;
        if (start[2] < brush.mins[2] - safeRadius && end[2] < brush.mins[2] - safeRadius)
            return;
        if (start[2] > brush.maxs[2] + safeRadius && end[2] > brush.maxs[2] + safeRadius)
            return;

        float enter = 0.0f;
        float exit = 1.0f;
        float enterNormal[3] = {0.0f, 0.0f, 1.0f};
        bool startsInside = true;
        bool enters = false;
        bool exits = false;
        for (uint32_t sideIndex = 0; sideIndex < brush.sideCount; ++sideIndex) {
            const BrushSide& side = sides_[brush.sideOffset + sideIndex];
            const float startDistance = Dot(side.plane.normal, start) - side.plane.dist - safeRadius;
            const float endDistance = Dot(side.plane.normal, end) - side.plane.dist - safeRadius;
            if (startDistance > 0.0f)
                startsInside = false;
            if (startDistance > 0.0f && endDistance <= 0.0f) {
                enters = true;
                const float denominator = startDistance - endDistance;
                if (denominator > 0.000001f) {
                    const float candidate = startDistance / denominator;
                    if (candidate > enter) {
                        enter = candidate;
                        enterNormal[0] = side.plane.normal[0];
                        enterNormal[1] = side.plane.normal[1];
                        enterNormal[2] = side.plane.normal[2];
                    }
                }
            } else if (startDistance <= 0.0f && endDistance > 0.0f) {
                const float denominator = startDistance - endDistance;
                if (denominator > 0.000001f) {
                    exit = std::min(exit, startDistance / denominator);
                    exits = true;
                }
            }
        }
        if (!startsInside && enters && enter <= exit && enter < result->fraction) {
            hit = true;
            result->fraction = std::max(0.0f, enter);
            result->normal[0] = enterNormal[0];
            result->normal[1] = enterNormal[1];
            result->normal[2] = enterNormal[2];
            result->contents = brush.contents;
        } else if (startsInside) {
            result->startSolid = true;
            result->fraction = 0.0f;
            result->contents = brush.contents;
            if (!exits)
                result->allSolid = true;
        }
    };

    // Light-grid corner and shadow queries are short line segments, so most
    // of their swept bounds touch only a handful of 256-unit cells.  Visit
    // only those cells and deduplicate brush indices with a reusable stamp
    // array.  If a query spans too much of the world, retain the exact old
    // full walk; the broadphase is an optimization and must never drop a
    // possible collision.
    if (!brushGridCells_.empty()
        && broadphaseVisitStamps_.size() == brushes_.size()) {
        ++broadphaseVisitStamp_;
        if (broadphaseVisitStamp_ == 0u) {
            std::fill(broadphaseVisitStamps_.begin(),
                      broadphaseVisitStamps_.end(), 0u);
            broadphaseVisitStamp_ = 1u;
        }
        for (const uint32_t brushIndex : broadphaseGlobalBrushes_)
            TestBrush(brushIndex);

        int32_t minimum[3]{};
        int32_t maximum[3]{};
        bool validBounds = true;
        for (uint32_t axis = 0; axis < 3u; ++axis) {
            const float low = std::min(start[axis], end[axis]) - safeRadius;
            const float high = std::max(start[axis], end[axis]) + safeRadius;
            if (!PositionToCell(low, &minimum[axis])
                || !PositionToCell(high, &maximum[axis])
                || maximum[axis] < minimum[axis]) {
                validBounds = false;
                break;
            }
        }
        const uint64_t spanX = validBounds
            ? static_cast<uint64_t>(static_cast<int64_t>(maximum[0])
                                    - minimum[0]) + 1u : 0u;
        const uint64_t spanY = validBounds
            ? static_cast<uint64_t>(static_cast<int64_t>(maximum[1])
                                    - minimum[1]) + 1u : 0u;
        const uint64_t spanZ = validBounds
            ? static_cast<uint64_t>(static_cast<int64_t>(maximum[2])
                                    - minimum[2]) + 1u : 0u;
        uint64_t queryCellCount = 0u;
        const bool queryCellCountValid = validBounds
            && ProductWithinLimit(spanX, spanY, spanZ,
                                  kBrushGridMaxQueryCells, &queryCellCount);
        const auto FindCell = [&](int32_t x, int32_t y, int32_t z) {
            size_t first = 0;
            size_t last = brushGridCells_.size();
            while (first < last) {
                const size_t middle = first + (last - first) / 2u;
                const BrushGridCell& cell = brushGridCells_[middle];
                if (CellBefore(cell.x, cell.y, cell.z, x, y, z)) {
                    first = middle + 1u;
                } else if (CellBefore(x, y, z,
                                      cell.x, cell.y, cell.z)) {
                    last = middle;
                } else {
                    return &cell;
                }
            }
            return static_cast<const BrushGridCell*>(nullptr);
        };
        const auto TestGridCell = [&](int64_t x, int64_t y, int64_t z) {
            if (x < std::numeric_limits<int32_t>::min()
                || x > std::numeric_limits<int32_t>::max()
                || y < std::numeric_limits<int32_t>::min()
                || y > std::numeric_limits<int32_t>::max()
                || z < std::numeric_limits<int32_t>::min()
                || z > std::numeric_limits<int32_t>::max())
                return;
            const BrushGridCell* cell = FindCell(
                static_cast<int32_t>(x), static_cast<int32_t>(y),
                static_cast<int32_t>(z));
            if (!cell)
                return;
            const uint64_t endIndex = static_cast<uint64_t>(cell->first)
                + cell->count;
            if (endIndex > brushGridBrushes_.size())
                return;
            for (uint32_t offset = 0; offset < cell->count; ++offset)
                TestBrush(brushGridBrushes_[cell->first + offset]);
        };
        if (queryCellCountValid && queryCellCount != 0u) {
            for (int64_t x = minimum[0]; x <= maximum[0]; ++x) {
                for (int64_t y = minimum[1]; y <= maximum[1]; ++y) {
                    for (int64_t z = minimum[2]; z <= maximum[2]; ++z) {
                        TestGridCell(x, y, z);
                    }
                }
            }
        } else {
            // Long source sun rays have a very large AABB even though they
            // cross only a thin sequence of cells. Walk that sequence with a
            // 3-D DDA instead of expanding the full ray envelope into a
            // cubic cell volume. Zero-radius rays are the source shadow
            // query; nonzero sweeps retain the exact full walk unless their
            // envelope fits the bounded cell query above.
            bool rayWalkComplete = false;
            if (validBounds && safeRadius <= 0.001f) {
                int64_t current[3]{};
                int64_t target[3]{};
                bool validRayCells = true;
                for (uint32_t axis = 0; axis < 3u; ++axis) {
                    int32_t startCell = 0;
                    int32_t endCell = 0;
                    if (!PositionToCell(start[axis], &startCell)
                        || !PositionToCell(end[axis], &endCell)) {
                        validRayCells = false;
                        break;
                    }
                    current[axis] = startCell;
                    target[axis] = endCell;
                }
                if (validRayCells) {
                    const double cellSize = static_cast<double>(
                        kBrushGridCellSize);
                    const double infinity = std::numeric_limits<double>::infinity();
                    double nextCrossing[3] = {infinity, infinity, infinity};
                    double crossingStep[3] = {infinity, infinity, infinity};
                    int64_t cellStep[3] = {0, 0, 0};
                    for (uint32_t axis = 0; axis < 3u; ++axis) {
                        const double delta = static_cast<double>(end[axis])
                            - static_cast<double>(start[axis]);
                        if (!std::isfinite(delta) || std::fabs(delta) <= 0.0000001)
                            continue;
                        if (delta > 0.0) {
                            cellStep[axis] = 1;
                            const double boundary =
                                static_cast<double>(current[axis] + 1) * cellSize;
                            nextCrossing[axis] = (boundary
                                - static_cast<double>(start[axis])) / delta;
                            crossingStep[axis] = cellSize / delta;
                        } else {
                            cellStep[axis] = -1;
                            const double boundary =
                                static_cast<double>(current[axis]) * cellSize;
                            nextCrossing[axis] = (static_cast<double>(start[axis])
                                - boundary) / -delta;
                            crossingStep[axis] = cellSize / -delta;
                        }
                        nextCrossing[axis] = std::max(nextCrossing[axis], 0.0);
                    }

                    for (uint32_t step = 0; step <= 4096u; ++step) {
                        TestGridCell(current[0], current[1], current[2]);
                        if (current[0] == target[0] && current[1] == target[1]
                            && current[2] == target[2]) {
                            rayWalkComplete = true;
                            break;
                        }
                        const double next = std::min(
                            nextCrossing[0],
                            std::min(nextCrossing[1], nextCrossing[2]));
                        if (!std::isfinite(next))
                            break;
                        bool advanced = false;
                        for (uint32_t axis = 0; axis < 3u; ++axis) {
                            if (nextCrossing[axis] <= next + 0.0000001) {
                                current[axis] += cellStep[axis];
                                nextCrossing[axis] += crossingStep[axis];
                                advanced = true;
                            }
                        }
                        if (!advanced)
                            break;
                    }
                }
            }
            if (!rayWalkComplete) {
                for (uint32_t brushIndex = 0;
                     brushIndex < static_cast<uint32_t>(brushes_.size());
                     ++brushIndex)
                    TestBrush(brushIndex);
            }
        }
    } else {
        for (uint32_t brushIndex = 0;
             brushIndex < static_cast<uint32_t>(brushes_.size());
             ++brushIndex)
            TestBrush(brushIndex);
    }
    return hit || result->startSolid;
}

bool ShipmentCollision::TraceLine(const float start[3], const float end[3],
                                  uint32_t contentsMask,
                                  ShipmentCollisionTrace* result) const {
    if (!loaded_ || !start || !end || !result)
        return false;

    // The convex brush query already returns the nearest brush hit and keeps
    // the source CONTENTS mask semantics.  Triangle soup has no per-triangle
    // contents in the compact VITC format, so it is treated as world-solid
    // when the caller asks for the normal world sight mask.
    const bool brushHit = TraceSphere(start, end, 0.0f, contentsMask, result);
    // A receiver sample is deliberately biased out of the rendered plane,
    // but the exported player brushes are thicker than that plane.  It is
    // therefore common for a valid light ray to begin inside its own floor,
    // wall, or roof brush.  Keep that state for the caller when the triangle
    // fallback finds no further occluder, but let the triangle soup continue
    // with a fresh nearest fraction so a roof/prop beyond the self brush can
    // still cast the source sun shadow.
    const bool brushStartSolid = result->startSolid;

    if (contentsMask && (contentsMask & 8193u) == 0u)
        return brushHit;

    const float direction[3] = {
        end[0] - start[0], end[1] - start[1], end[2] - start[2],
    };
    const float directionLengthSquared = direction[0] * direction[0]
        + direction[1] * direction[1] + direction[2] * direction[2];
    if (!std::isfinite(directionLengthSquared)
        || directionLengthSquared <= 0.000001f)
        return brushHit;

    const auto Cross = [](const float a[3], const float b[3], float output[3]) {
        output[0] = a[1] * b[2] - a[2] * b[1];
        output[1] = a[2] * b[0] - a[0] * b[2];
        output[2] = a[0] * b[1] - a[1] * b[0];
    };
    float nearestFraction = brushStartSolid ? 1.0f : result->fraction;
    bool triangleHit = false;
    float nearestNormal[3] = {0.0f, 0.0f, 1.0f};
    for (const auto& triangle : triangles_) {
        const auto& a = vertices_[triangle[0]];
        const auto& b = vertices_[triangle[1]];
        const auto& c = vertices_[triangle[2]];
        const float edge1[3] = {
            b[0] - a[0], b[1] - a[1], b[2] - a[2],
        };
        const float edge2[3] = {
            c[0] - a[0], c[1] - a[1], c[2] - a[2],
        };
        float pvec[3]{};
        Cross(direction, edge2, pvec);
        const float determinant = edge1[0] * pvec[0]
            + edge1[1] * pvec[1] + edge1[2] * pvec[2];
        if (!std::isfinite(determinant) || std::fabs(determinant) <= 0.000001f)
            continue;
        const float inverseDeterminant = 1.0f / determinant;
        const float toStart[3] = {
            start[0] - a[0], start[1] - a[1], start[2] - a[2],
        };
        const float barycentricU = (toStart[0] * pvec[0]
            + toStart[1] * pvec[1] + toStart[2] * pvec[2])
            * inverseDeterminant;
        if (barycentricU < -0.0001f || barycentricU > 1.0001f)
            continue;
        float qvec[3]{};
        Cross(toStart, edge1, qvec);
        const float barycentricV = (direction[0] * qvec[0]
            + direction[1] * qvec[1] + direction[2] * qvec[2])
            * inverseDeterminant;
        if (barycentricV < -0.0001f
            || barycentricU + barycentricV > 1.0001f)
            continue;
        const float fraction = (edge2[0] * qvec[0]
            + edge2[1] * qvec[1] + edge2[2] * qvec[2])
            * inverseDeterminant;
        // The caller offsets a receiver away from its own plane. Ignore a
        // numerically coincident hit so a surface cannot shadow itself.
        if (!std::isfinite(fraction) || fraction <= 0.0001f
            || fraction >= nearestFraction)
            continue;
        float normal[3]{};
        Cross(edge1, edge2, normal);
        const float normalLengthSquared = normal[0] * normal[0]
            + normal[1] * normal[1] + normal[2] * normal[2];
        if (!std::isfinite(normalLengthSquared)
            || normalLengthSquared <= 0.000001f)
            continue;
        const float inverseNormalLength = 1.0f / std::sqrt(normalLengthSquared);
        for (float& component : normal)
            component *= inverseNormalLength;
        if (normal[0] * direction[0] + normal[1] * direction[1]
            + normal[2] * direction[2] > 0.0f) {
            for (float& component : normal)
                component = -component;
        }
        nearestFraction = fraction;
        std::memcpy(nearestNormal, normal, sizeof(nearestNormal));
        triangleHit = true;
    }
    if (triangleHit) {
        result->fraction = nearestFraction;
        std::memcpy(result->normal, nearestNormal, sizeof(result->normal));
        result->contents = 8193u;
        // The brush start-solid was the receiver's own collision volume, not
        // the occluder that the directional-light query found.  Clear it so
        // the shadow caller can consume this valid triangle hit.
        result->startSolid = false;
        result->allSolid = false;
    }
    return brushHit || triangleHit;
}

bool ShipmentCollision::TraceLineBrushesOnly(
    const float start[3], const float end[3], uint32_t contentsMask,
    ShipmentCollisionTrace* result) const {
    // CM_BoxSightTrace walks the BSP/brush sight path.  The VITC mesh stream
    // is retained for triangle-only movement and future leaf-aware queries,
    // but it has no leaf/content metadata with which to reproduce that
    // source visibility decision globally.
    return TraceSphere(start, end, 0.0f, contentsMask, result);
}

bool ShipmentCollision::TraceCapsule(const float start[3], const float mins[3],
                                     const float maxs[3], const float end[3],
                                     uint32_t contentsMask,
                                     ShipmentCollisionTrace* result) const {
    if (!loaded_ || !start || !mins || !maxs || !end || !result)
        return false;
    *result = ShipmentCollisionTrace{};
    result->fraction = 1.0f;
    const float broadRadius = std::max({std::fabs(mins[0]), std::fabs(maxs[0]),
                                        std::fabs(mins[1]), std::fabs(maxs[1]),
                                        std::fabs(mins[2]), std::fabs(maxs[2])});
    bool hit = false;

    // The Shipment sidecar carries the map's triangle soup in addition to
    // convex brushes.  Floors and ramps are commonly triangle-only; retain a
    // small upward-surface sweep so the shared COD4 pmove can land on them
    // without pulling native clipMap pointers into the Vita build.  This path
    // intentionally handles downward player sweeps (the ground/step queries)
    // and leaves wall/ceiling response to the brush planes below.
    const float startBottom = start[2] + mins[2];
    const float endBottom = end[2] + mins[2];
    if (endBottom < startBottom - 0.0001f && !triangles_.empty()) {
        const float dx = end[0] - start[0];
        const float dy = end[1] - start[1];
        for (const auto& triangle : triangles_) {
            const auto& a = vertices_[triangle[0]];
            const auto& b = vertices_[triangle[1]];
            const auto& c = vertices_[triangle[2]];
            const float abx = b[0] - a[0];
            const float aby = b[1] - a[1];
            const float abz = b[2] - a[2];
            const float acx = c[0] - a[0];
            const float acy = c[1] - a[1];
            const float acz = c[2] - a[2];
            float nx = aby * acz - abz * acy;
            float ny = abz * acx - abx * acz;
            float nz = abx * acy - aby * acx;
            if (std::fabs(nz) < 0.7f)
                continue;
            if (nz < 0.0f) {
                nx = -nx;
                ny = -ny;
                nz = -nz;
            }
            const float normalLength = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (normalLength < 0.0001f)
                continue;
            nx /= normalLength;
            ny /= normalLength;
            nz /= normalLength;
            const float floorZ = a[2] - (nx * (start[0] - a[0])
                                        + ny * (start[1] - a[1])) / nz;
            const float startDistance = startBottom - floorZ;
            const float endFloorZ = a[2] - (nx * (end[0] - a[0])
                                          + ny * (end[1] - a[1])) / nz;
            const float endDistance = endBottom - endFloorZ;
            // A grounded player can arrive a few sub-units below the
            // exported triangle plane because shared Pmove applies its
            // 0.25-unit ground probe after integrating velocity.  Treat that
            // near-plane/penetrating downward probe as an immediate contact
            // instead of letting gravity accumulate through the floor.
            if (endDistance > 0.001f)
                continue;
            float candidate = 0.0f;
            if (startDistance > 0.001f) {
                const float denominator = startDistance - endDistance;
                if (denominator <= 0.0001f)
                    continue;
                candidate = std::clamp(startDistance / denominator, 0.0f, 1.0f);
            }
            const float px = start[0] + dx * candidate;
            const float py = start[1] + dy * candidate;
            const float v0x = c[0] - a[0];
            const float v0y = c[1] - a[1];
            const float v1x = b[0] - a[0];
            const float v1y = b[1] - a[1];
            const float v2x = px - a[0];
            const float v2y = py - a[1];
            const float denom = v0x * v1y - v1x * v0y;
            if (std::fabs(denom) < 0.0001f)
                continue;
            const float u = (v2x * v1y - v1x * v2y) / denom;
            const float v = (v0x * v2y - v2x * v0y) / denom;
            if (u < -0.02f || v < -0.02f || u + v > 1.02f)
                continue;
            if (candidate < result->fraction) {
                hit = true;
                result->fraction = candidate;
                result->normal[0] = nx;
                result->normal[1] = ny;
                result->normal[2] = nz;
                result->contents = 0x08000001u;
            }
        }
    }

    for (const Brush& brush : brushes_) {
        if (brush.sideCount == 0)
            continue;
        if (contentsMask && !(brush.contents & contentsMask))
            continue;
        if (start[0] < brush.mins[0] - broadRadius && end[0] < brush.mins[0] - broadRadius)
            continue;
        if (start[0] > brush.maxs[0] + broadRadius && end[0] > brush.maxs[0] + broadRadius)
            continue;
        if (start[1] < brush.mins[1] - broadRadius && end[1] < brush.mins[1] - broadRadius)
            continue;
        if (start[1] > brush.maxs[1] + broadRadius && end[1] > brush.maxs[1] + broadRadius)
            continue;
        if (start[2] < brush.mins[2] - broadRadius && end[2] < brush.mins[2] - broadRadius)
            continue;
        if (start[2] > brush.maxs[2] + broadRadius && end[2] > brush.maxs[2] + broadRadius)
            continue;

        float enter = 0.0f;
        float exit = 1.0f;
        float enterNormal[3] = {0.0f, 0.0f, 1.0f};
        bool startsInside = true;
        bool enters = false;
        bool exits = false;
        for (uint32_t sideIndex = 0; sideIndex < brush.sideCount; ++sideIndex) {
            const BrushSide& side = sides_[brush.sideOffset + sideIndex];
            const float support =
                side.plane.normal[0] * (side.plane.normal[0] >= 0.0f ? maxs[0] : mins[0])
                + side.plane.normal[1] * (side.plane.normal[1] >= 0.0f ? maxs[1] : mins[1])
                + side.plane.normal[2] * (side.plane.normal[2] >= 0.0f ? maxs[2] : mins[2]);
            const float startDistance = Dot(side.plane.normal, start) - side.plane.dist - support;
            const float endDistance = Dot(side.plane.normal, end) - side.plane.dist - support;
            if (startDistance > 0.0f)
                startsInside = false;
            if (startDistance > 0.0f && endDistance <= 0.0f) {
                enters = true;
                const float denominator = startDistance - endDistance;
                if (denominator > 0.000001f) {
                    const float candidate = startDistance / denominator;
                    if (candidate > enter) {
                        enter = candidate;
                        enterNormal[0] = side.plane.normal[0];
                        enterNormal[1] = side.plane.normal[1];
                        enterNormal[2] = side.plane.normal[2];
                    }
                }
            } else if (startDistance <= 0.0f && endDistance > 0.0f) {
                const float denominator = startDistance - endDistance;
                if (denominator > 0.000001f) {
                    exit = std::min(exit, startDistance / denominator);
                    exits = true;
                }
            }
        }
        if (!startsInside && enters && enter <= exit && enter < result->fraction) {
            hit = true;
            result->fraction = std::max(0.0f, enter);
            result->normal[0] = enterNormal[0];
            result->normal[1] = enterNormal[1];
            result->normal[2] = enterNormal[2];
            result->contents = brush.contents;
        } else if (startsInside) {
            result->startSolid = true;
            result->fraction = 0.0f;
            result->contents = brush.contents;
            if (!exits)
                result->allSolid = true;
        }
    }
    return hit || result->startSolid;
}

} // namespace kisak::vita
