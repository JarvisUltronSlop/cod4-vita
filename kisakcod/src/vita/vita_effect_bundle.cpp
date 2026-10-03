#include <vita/vita_effect_bundle.h>

#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x41584656; // "VFXA"
constexpr uint32_t kVersion1 = 1;
constexpr uint32_t kVersion2 = 2;
constexpr uint32_t kVersion3 = 3;
constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kMaxStringBytes = 1u * 1024u * 1024u;
constexpr uint32_t kMaxNameBytes = 1024;
constexpr uint32_t kMaxPathBytes = 2048;
constexpr uint32_t kMaxSoundBytes = 1024;
constexpr uint64_t kMaxAggregateBytes = 4ull * 1024ull * 1024ull;
constexpr uint32_t kMaxGraphElements = 131072;
constexpr uint32_t kMaxGraphVisuals = 131072;
constexpr uint32_t kMaxGraphVelocitySamples = 262144;
constexpr uint32_t kMaxGraphVisualSamples = 262144;
constexpr uint32_t kMaxGraphTrailVertices = 131072;
constexpr uint32_t kMaxGraphTrailIndices = 262144;
constexpr size_t kHeaderV1Bytes = 32;
constexpr size_t kHeaderV2Bytes = 64;
constexpr size_t kDefinitionBytes = 8;
constexpr size_t kPlacementBytes = 36;
constexpr size_t kLoopSoundBytes = 28;
constexpr size_t kGraphEffectBytes = 36;
constexpr size_t kGraphElementV2Bytes = 316;
constexpr size_t kGraphElementV3Bytes = 332;
constexpr size_t kGraphVisualBytes = 4;
constexpr size_t kGraphVelocitySampleBytes = 96;
constexpr size_t kGraphVisualSampleBytes = 48;
constexpr size_t kGraphTrailVertexBytes = 20;
constexpr size_t kGraphTrailIndexBytes = 2;

uint32_t ReadU32(const uint8_t* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

float ReadFloat(const uint8_t* bytes) {
    float value = 0.0f;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

int32_t ReadI32(const uint8_t* bytes) {
    int32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool Finite(const float* values, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index]))
            return false;
    }
    return true;
}

bool FiniteElement(const VitaEffectElement& element) {
    const auto finite = [](const float* values, size_t count) {
        return Finite(values, count);
    };
    return finite(element.spawnRange, 2)
        && finite(element.fadeInRange, 2)
        && finite(element.fadeOutRange, 2)
        && finite(&element.spawnFrustumCullRadius, 1)
        && finite(&element.spawnOrigin[0][0], 6)
        && finite(element.spawnOffsetRadius, 2)
        && finite(element.spawnOffsetHeight, 2)
        && finite(&element.spawnAngles[0][0], 6)
        && finite(&element.angularVelocity[0][0], 6)
        && finite(element.initialRotation, 2)
        && finite(element.gravity, 2)
        && finite(element.reflectionFactor, 2)
        && finite(element.collMins, 3)
        && finite(element.collMaxs, 3)
        && finite(element.emitDist, 2)
        && finite(element.emitDistVariance, 2)
        && finite(&element.lightingFrac, 1);
}

bool GraphBytesForCounts(uint32_t effectCount, uint32_t elementCount,
                         uint32_t visualCount, uint32_t velocitySampleCount,
                         uint32_t visualSampleCount, uint32_t trailVertexCount,
                         uint32_t trailIndexCount, size_t graphElementBytes,
                         uint64_t* bytes) {
    if (!bytes || effectCount > kMaxRecords
        || elementCount > kMaxGraphElements || visualCount > kMaxGraphVisuals
        || velocitySampleCount > kMaxGraphVelocitySamples
        || visualSampleCount > kMaxGraphVisualSamples
        || trailVertexCount > kMaxGraphTrailVertices
        || trailIndexCount > kMaxGraphTrailIndices)
        return false;
    const uint64_t total = static_cast<uint64_t>(effectCount)
        * kGraphEffectBytes + static_cast<uint64_t>(elementCount)
        * graphElementBytes + static_cast<uint64_t>(visualCount)
        * kGraphVisualBytes + static_cast<uint64_t>(velocitySampleCount)
        * kGraphVelocitySampleBytes + static_cast<uint64_t>(visualSampleCount)
        * kGraphVisualSampleBytes + static_cast<uint64_t>(trailVertexCount)
        * kGraphTrailVertexBytes + static_cast<uint64_t>(trailIndexCount)
        * kGraphTrailIndexBytes;
    if (total > kMaxAggregateBytes)
        return false;
    *bytes = total;
    return true;
}

bool ReadStringAt(const std::vector<uint8_t>& data, size_t stringBase,
                 uint32_t stringBytes, uint32_t offset, uint32_t maximum,
                 bool allowEmpty, std::string* output) {
    if (!output || offset >= stringBytes || stringBase > data.size()
        || stringBytes > data.size() - stringBase)
        return false;
    const uint8_t* begin = data.data() + stringBase + offset;
    const size_t remaining = stringBytes - offset;
    size_t length = 0;
    while (length < remaining && begin[length] != 0)
        ++length;
    if (length == remaining || length > maximum || (!allowEmpty && length == 0))
        return false;
    output->assign(reinterpret_cast<const char*>(begin), length);
    return true;
}

bool ReadGraphElement(const std::vector<uint8_t>& data, size_t recordBase,
                      size_t visualBase, size_t velocityBase,
                      size_t visualSampleBase, size_t trailVertexBase,
                      size_t trailIndexBase, size_t stringBase,
                      uint32_t stringBytes, uint32_t graphVisualCount,
                      uint32_t graphVelocitySampleCount,
                      uint32_t graphVisualSampleCount,
                       uint32_t graphTrailVertexCount,
                       uint32_t graphTrailIndexCount,
                       uint32_t version,
                       VitaEffectElement* output) {
    const size_t recordBytes = version == kVersion3
        ? kGraphElementV3Bytes : kGraphElementV2Bytes;
    if (!output || recordBase > data.size()
        || recordBytes > data.size() - recordBase)
        return false;
    const uint8_t* record = data.data() + recordBase;
    const uint32_t visualOffset = ReadU32(record + 20);
    const uint32_t visualRefCount = ReadU32(record + 24);
    const uint32_t velocityOffset = ReadU32(record + 28);
    const uint32_t velocityCount = ReadU32(record + 32);
    const uint32_t visualSampleOffset = ReadU32(record + 36);
    const uint32_t visualSampleCount = ReadU32(record + 40);
    const uint32_t trailVertexOffset = ReadU32(record + 44);
    const uint32_t trailVertexCount = ReadU32(record + 48);
    const uint32_t trailIndexOffset = ReadU32(record + 52);
    const uint32_t trailIndexCount = ReadU32(record + 56);
    const auto within = [](uint32_t offset, uint32_t count,
                           uint32_t total) {
        return offset <= total && count <= total - offset;
    };
    if (visualRefCount > 64 || !within(visualOffset, visualRefCount,
                                    graphVisualCount)
        || !within(velocityOffset, velocityCount,
                   graphVelocitySampleCount)
        || !within(visualSampleOffset, visualSampleCount,
                   graphVisualSampleCount)
        || !within(trailVertexOffset, trailVertexCount,
                   graphTrailVertexCount)
        || !within(trailIndexOffset, trailIndexCount,
                   graphTrailIndexCount))
        return false;

    *output = VitaEffectElement{};
    output->flags = ReadU32(record + 0);
    output->elemType = ReadU32(record + 4);
    output->visualCount = ReadU32(record + 8);
    output->velIntervalCount = ReadU32(record + 12);
    output->visStateIntervalCount = ReadU32(record + 16);
    if (output->visualCount > 32 || output->elemType > 10u
        || output->velIntervalCount > 80u
        || output->visStateIntervalCount > 80u)
        return false;
    output->lightingFrac = ReadFloat(record + 80);
    for (size_t index = 0; index < 2; ++index) {
        output->spawnRange[index] = ReadFloat(record + 84 + index * 4);
        output->fadeInRange[index] = ReadFloat(record + 92 + index * 4);
        output->fadeOutRange[index] = ReadFloat(record + 100 + index * 4);
        output->spawnDelayMsec[index] =
            ReadI32(record + 112 + index * sizeof(int32_t));
        output->lifeSpanMsec[index] =
            ReadI32(record + 120 + index * sizeof(int32_t));
        output->spawnOffsetRadius[index] =
            ReadFloat(record + 152 + index * 4);
        output->spawnOffsetHeight[index] =
            ReadFloat(record + 160 + index * 4);
        output->initialRotation[index] = ReadFloat(record + 216 + index * 4);
        output->gravity[index] = ReadFloat(record + 224 + index * 4);
        output->reflectionFactor[index] =
            ReadFloat(record + 232 + index * 4);
        output->emitDist[index] = ReadFloat(record + 292 + index * 4);
        output->emitDistVariance[index] =
            ReadFloat(record + 300 + index * 4);
        output->spawnDef[index] =
            ReadI32(record + 308 + index * sizeof(int32_t));
    }
    output->spawnFrustumCullRadius = ReadFloat(record + 108);
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t component = 0; component < 2; ++component) {
            output->spawnOrigin[axis][component] =
                ReadFloat(record + 128 + (axis * 2 + component) * 4);
            output->spawnAngles[axis][component] =
                ReadFloat(record + 168 + (axis * 2 + component) * 4);
            output->angularVelocity[axis][component] =
                ReadFloat(record + 192 + (axis * 2 + component) * 4);
        }
    }
    for (size_t component = 0; component < 3; ++component) {
        output->collMins[component] = ReadFloat(record + 268 + component * 4);
        output->collMaxs[component] = ReadFloat(record + 280 + component * 4);
    }
    const uint32_t atlasBehavior = ReadU32(record + 240);
    const uint32_t atlasIndex = ReadU32(record + 244);
    const uint32_t atlasFps = ReadU32(record + 248);
    const uint32_t atlasLoopCount = ReadU32(record + 252);
    const uint32_t atlasColIndexBits = ReadU32(record + 256);
    const uint32_t atlasRowIndexBits = ReadU32(record + 260);
    if (atlasBehavior > UINT8_MAX || atlasIndex > UINT8_MAX
        || atlasFps > UINT8_MAX || atlasLoopCount > UINT8_MAX
        || atlasColIndexBits > UINT8_MAX || atlasRowIndexBits > UINT8_MAX)
        return false;
    output->atlasBehavior = static_cast<uint8_t>(atlasBehavior);
    output->atlasIndex = static_cast<uint8_t>(atlasIndex);
    output->atlasFps = static_cast<uint8_t>(atlasFps);
    output->atlasLoopCount = static_cast<uint8_t>(atlasLoopCount);
    output->atlasColIndexBits = static_cast<uint8_t>(atlasColIndexBits);
    output->atlasRowIndexBits = static_cast<uint8_t>(atlasRowIndexBits);
    output->atlasEntryCount = ReadI32(record + 264);
    output->sortOrder = ReadU32(record + 72);
    output->useItemClip = ReadU32(record + 76);
    if (output->atlasEntryCount < INT16_MIN
        || output->atlasEntryCount > INT16_MAX
        || output->sortOrder > UINT8_MAX || output->useItemClip > UINT8_MAX
        || output->lightingFrac < 0.0f || output->lightingFrac > 255.0f)
        return false;

    if (version == kVersion3) {
        output->trailScrollTimeMsec = ReadI32(record + 316);
        output->trailRepeatDist = ReadI32(record + 320);
        output->trailSplitDist = ReadI32(record + 324);
        const uint32_t trailPresent = ReadU32(record + 328);
        if (trailPresent > 1u)
            return false;
        output->trailPresent = trailPresent != 0;
    }

    auto readRef = [&](size_t offset, std::string* value) {
        return ReadStringAt(data, stringBase, stringBytes,
                            ReadU32(record + offset), kMaxPathBytes, true,
                            value);
    };
    if (!readRef(60, &output->effectOnImpact)
        || !readRef(64, &output->effectOnDeath)
        || !readRef(68, &output->effectEmitted))
        return false;

    output->visualNames.reserve(visualRefCount);
    for (uint32_t index = 0; index < visualRefCount; ++index) {
        if (visualBase > data.size()
            || (static_cast<uint64_t>(index) + visualOffset)
                   * kGraphVisualBytes > data.size() - visualBase)
            return false;
        std::string visual;
        const uint32_t offset = ReadU32(
            data.data() + visualBase
            + (static_cast<size_t>(visualOffset) + index) * kGraphVisualBytes);
        const bool emptyMarkSlot = version == kVersion3
            && output->elemType == 9u;
        if (!ReadStringAt(data, stringBase, stringBytes, offset,
                          kMaxPathBytes, emptyMarkSlot, &visual))
            return false;
        output->visualNames.push_back(std::move(visual));
    }

    output->velocitySamples.resize(velocityCount);
    for (uint32_t index = 0; index < velocityCount; ++index) {
        if (velocityBase > data.size()
            || (static_cast<uint64_t>(index) + velocityOffset)
                   * kGraphVelocitySampleBytes > data.size() - velocityBase)
            return false;
        auto& sample = output->velocitySamples[index];
        const uint8_t* source = data.data() + velocityBase
            + (static_cast<size_t>(velocityOffset) + index)
                * kGraphVelocitySampleBytes;
        for (size_t value = 0; value < sample.size(); ++value) {
            sample[value] = ReadFloat(source + value * sizeof(float));
            if (!std::isfinite(sample[value]))
                return false;
        }
    }

    output->visualSamples.resize(visualSampleCount);
    for (uint32_t index = 0; index < visualSampleCount; ++index) {
        if (visualSampleBase > data.size()
            || (static_cast<uint64_t>(index) + visualSampleOffset)
                   * kGraphVisualSampleBytes > data.size() - visualSampleBase)
            return false;
        const uint8_t* source = data.data() + visualSampleBase
            + (static_cast<size_t>(visualSampleOffset) + index)
                * kGraphVisualSampleBytes;
        VitaEffectVisualSample& sample = output->visualSamples[index];
        std::memcpy(sample.baseColor, source, sizeof(sample.baseColor));
        sample.baseRotationDelta = ReadFloat(source + 4);
        sample.baseRotationTotal = ReadFloat(source + 8);
        sample.baseSize[0] = ReadFloat(source + 12);
        sample.baseSize[1] = ReadFloat(source + 16);
        sample.baseScale = ReadFloat(source + 20);
        std::memcpy(sample.amplitudeColor, source + 24,
                    sizeof(sample.amplitudeColor));
        sample.amplitudeRotationDelta = ReadFloat(source + 28);
        sample.amplitudeRotationTotal = ReadFloat(source + 32);
        sample.amplitudeSize[0] = ReadFloat(source + 36);
        sample.amplitudeSize[1] = ReadFloat(source + 40);
        sample.amplitudeScale = ReadFloat(source + 44);
        if (!Finite(&sample.baseRotationDelta, 5)
            || !Finite(&sample.amplitudeRotationDelta, 5))
            return false;
    }

    output->trailVertices.resize(trailVertexCount);
    for (uint32_t index = 0; index < trailVertexCount; ++index) {
        if (trailVertexBase > data.size()
            || (static_cast<uint64_t>(index) + trailVertexOffset)
                   * kGraphTrailVertexBytes > data.size() - trailVertexBase)
            return false;
        const uint8_t* source = data.data() + trailVertexBase
            + (static_cast<size_t>(trailVertexOffset) + index)
                * kGraphTrailVertexBytes;
        auto& vertex = output->trailVertices[index];
        for (size_t value = 0; value < vertex.size(); ++value) {
            vertex[value] = ReadFloat(source + value * sizeof(float));
            if (!std::isfinite(vertex[value]))
                return false;
        }
    }
    output->trailIndices.resize(trailIndexCount);
    for (uint32_t index = 0; index < trailIndexCount; ++index) {
        if (trailIndexBase > data.size()
            || (static_cast<uint64_t>(index) + trailIndexOffset)
                   * kGraphTrailIndexBytes > data.size() - trailIndexBase)
            return false;
        std::memcpy(&output->trailIndices[index],
                    data.data() + trailIndexBase
                        + (static_cast<size_t>(trailIndexOffset) + index)
                            * kGraphTrailIndexBytes,
                    sizeof(uint16_t));
        if (output->trailIndices[index] >= trailVertexCount)
            return false;
    }
    if (version == kVersion3) {
        if (output->trailPresent != (output->elemType == 3u)
            || (!output->trailPresent
                && (!output->trailVertices.empty()
                    || !output->trailIndices.empty()
                    || output->trailScrollTimeMsec != 0
                    || output->trailRepeatDist != 0
                    || output->trailSplitDist != 0)))
            return false;
    } else {
        output->trailPresent = !output->trailVertices.empty()
            || !output->trailIndices.empty();
    }
    return FiniteElement(*output);
}

bool IsRainEffect(const VitaEffectRecord& effect) {
    constexpr std::string_view kWeatherPrefix = "weather/";
    return effect.path.rfind(kWeatherPrefix.data(), 0) == 0
        && effect.path.find("rain") != std::string::npos;
}

VitaEffectVisualClass ClassifyEffectPath(const VitaEffectRecord& effect) {
    const std::string_view path(effect.path);
    if (IsRainEffect(effect) || path.find("weather/") == 0)
        return VitaEffectVisualClass::Weather;
    if (path.find("smoke/") == 0 || path.find("smoke") != std::string_view::npos)
        return VitaEffectVisualClass::Smoke;
    if (path.find("dust/") == 0 || path.find("ash") != std::string_view::npos)
        return VitaEffectVisualClass::Dust;
    if (path.find("fire/") == 0 || path.find("fire") != std::string_view::npos)
        return VitaEffectVisualClass::Fire;
    if (path.find("paper") != std::string_view::npos)
        return VitaEffectVisualClass::Paper;
    if (path.find("water") != std::string_view::npos
        || path.find("stream") != std::string_view::npos
        || path.find("drip") != std::string_view::npos
        || path.find("rain_splash") != std::string_view::npos)
        return VitaEffectVisualClass::Water;
    if (path.find("leaf") != std::string_view::npos
        || path.find("insect") != std::string_view::npos
        || path.find("hawk") != std::string_view::npos
        || path.find("bird") != std::string_view::npos)
        return VitaEffectVisualClass::Foliage;
    if (path.find("light") != std::string_view::npos
        || path.find("glow") != std::string_view::npos
        || path.find("fluorescent") != std::string_view::npos)
        return VitaEffectVisualClass::Light;
    return VitaEffectVisualClass::Other;
}

uint32_t HashMix(uint32_t hash, uint32_t value) {
    // FNV-1a keeps the source-anchor seed stable across Vita/compiler builds
    // without exposing a pointer or relying on std::hash's implementation.
    hash ^= value;
    return hash * 16777619u;
}

uint32_t FloatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

} // namespace

void VitaEffectBundle::Clear() {
    mapIndex_ = 0;
    effects_.clear();
    placements_.clear();
    loopSounds_.clear();
    loaded_ = false;
    wireVersion_ = 0;
}

bool VitaEffectBundle::SourceComplete() const {
    if (!loaded_ || wireVersion_ != kVersion3 || effects_.empty())
        return false;
    for (const VitaEffectRecord& effect : effects_) {
        const uint64_t elementCount = static_cast<uint64_t>(
            effect.elemDefCountLooping) + effect.elemDefCountOneShot
            + effect.elemDefCountEmission;
        if (!effect.graphPresent || effect.name.empty() || effect.path.empty()
            || elementCount != effect.elements.size()
            || elementCount > 32u)
            return false;
        for (const VitaEffectRecord& other : effects_) {
            if (&other != &effect && other.path == effect.path)
                return false;
        }
        for (const VitaEffectElement& element : effect.elements) {
            const size_t expectedVisuals = element.elemType == 9u
                ? static_cast<size_t>(element.visualCount) * 2u
                : element.elemType == 6u || element.elemType == 7u
                ? 0u : element.visualCount;
            const size_t expectedVisualSamples = element.elemType == 10u
                ? 0u : static_cast<size_t>(element.visStateIntervalCount) + 1u;
            if (element.elemType > 10u || element.visualCount > 32u
                || element.velIntervalCount > 80u
                || element.visStateIntervalCount > 80u
                || element.visualNames.size() != expectedVisuals
                || element.velocitySamples.size()
                    != static_cast<size_t>(element.velIntervalCount) + 1u
                || element.visualSamples.size() != expectedVisualSamples
                || element.trailPresent != (element.elemType == 3u)
                || std::floor(element.lightingFrac) != element.lightingFrac)
                return false;
            if (element.elemType != 9u) {
                for (const std::string& visual : element.visualNames) {
                    if (visual.empty())
                        return false;
                }
            }
        }
    }
    return true;
}

bool VitaEffectBundle::Load(const VitaAssetDatabase& database, uint64_t assetId) {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::Effects, assetId) : nullptr;
    if (assetId && !requested)
        return false;
    for (uint32_t chunkIndex = 0; chunkIndex < database.ChunkCount(); ++chunkIndex) {
        const KvapChunk* chunk = database.Chunk(chunkIndex);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Effects)
            || (assetId && chunk != requested)
            || chunk->rawSize < kHeaderV1Bytes
            || chunk->rawSize > kMaxAggregateBytes)
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, data.data(), data.size()) != KvapError::Ok
            || ReadU32(data.data()) != kMagic
            || (ReadU32(data.data() + 4) != kVersion1
                && ReadU32(data.data() + 4) != kVersion2
                && ReadU32(data.data() + 4) != kVersion3))
            continue;
        const uint32_t version = ReadU32(data.data() + 4);
        const size_t headerBytes = version >= kVersion2
            ? kHeaderV2Bytes : kHeaderV1Bytes;
        const size_t graphElementBytes = version == kVersion3
            ? kGraphElementV3Bytes : kGraphElementV2Bytes;
        if (data.size() < headerBytes)
            continue;
        const uint32_t mapIndex = ReadU32(data.data() + 8);
        const uint32_t effectCount = ReadU32(data.data() + 12);
        const uint32_t placementCount = ReadU32(data.data() + 16);
        const uint32_t loopSoundCount = ReadU32(data.data() + 20);
        const uint32_t stringBytes = ReadU32(data.data() + 24);
        uint32_t graphEffectCount = 0;
        uint32_t graphElementCount = 0;
        uint32_t graphVisualCount = 0;
        uint32_t graphVelocitySampleCount = 0;
        uint32_t graphVisualSampleCount = 0;
        uint32_t graphTrailVertexCount = 0;
        uint32_t graphTrailIndexCount = 0;
        uint32_t graphBytes = 0;
        const uint32_t reserved = version >= kVersion2
            ? ReadU32(data.data() + 60) : ReadU32(data.data() + 28);
        if (version >= kVersion2) {
            graphEffectCount = ReadU32(data.data() + 28);
            graphElementCount = ReadU32(data.data() + 32);
            graphVisualCount = ReadU32(data.data() + 36);
            graphVelocitySampleCount = ReadU32(data.data() + 40);
            graphVisualSampleCount = ReadU32(data.data() + 44);
            graphTrailVertexCount = ReadU32(data.data() + 48);
            graphTrailIndexCount = ReadU32(data.data() + 52);
            graphBytes = ReadU32(data.data() + 56);
        }
        if (effectCount == 0 || effectCount > kMaxRecords
            || placementCount > kMaxRecords || loopSoundCount > kMaxRecords
            || stringBytes == 0 || stringBytes > kMaxStringBytes
            || reserved != 0)
            continue;
        uint64_t expectedGraphBytes = 0;
        if (!GraphBytesForCounts(
                graphEffectCount, graphElementCount, graphVisualCount,
                graphVelocitySampleCount, graphVisualSampleCount,
                graphTrailVertexCount, graphTrailIndexCount,
                graphElementBytes,
                &expectedGraphBytes)
            || expectedGraphBytes != graphBytes)
            continue;
        if (graphEffectCount > effectCount)
            continue;
        if (graphEffectCount == 0
            && (graphElementCount != 0 || graphVisualCount != 0
                || graphVelocitySampleCount != 0
                || graphVisualSampleCount != 0
                || graphTrailVertexCount != 0 || graphTrailIndexCount != 0
                || graphBytes != 0))
            continue;
        const uint64_t recordsBytes = headerBytes
            + static_cast<uint64_t>(effectCount) * kDefinitionBytes
            + static_cast<uint64_t>(placementCount) * kPlacementBytes
            + static_cast<uint64_t>(loopSoundCount) * kLoopSoundBytes;
        if (recordsBytes + stringBytes + graphBytes != data.size())
            continue;

        const size_t definitionsOffset = headerBytes;
        const size_t placementsOffset = definitionsOffset
            + static_cast<size_t>(effectCount) * kDefinitionBytes;
        const size_t soundsOffset = placementsOffset
            + static_cast<size_t>(placementCount) * kPlacementBytes;
        const size_t stringBase = soundsOffset
            + static_cast<size_t>(loopSoundCount) * kLoopSoundBytes;
        std::vector<VitaEffectRecord> effects;
        effects.reserve(effectCount);
        bool valid = true;
        for (uint32_t index = 0; index < effectCount; ++index) {
            const uint8_t* record = data.data() + definitionsOffset
                + static_cast<size_t>(index) * kDefinitionBytes;
            const uint32_t nameOffset = ReadU32(record);
            const uint32_t pathOffset = ReadU32(record + 4);
            VitaEffectRecord effect;
            // An empty path is retained as an explicit missing-source marker;
            // the package report/runtime trace can identify it instead of
            // silently binding an unrelated material.
            valid = ReadStringAt(data, stringBase, stringBytes, nameOffset,
                                 kMaxNameBytes, false, &effect.name);
            if (valid && pathOffset != 0)
                valid = ReadStringAt(data, stringBase, stringBytes, pathOffset,
                                     kMaxPathBytes, false, &effect.path);
            if (!valid)
                break;
            effects.push_back(std::move(effect));
        }
        if (!valid)
            continue;

        std::vector<VitaEffectPlacement> placements;
        placements.reserve(placementCount);
        for (uint32_t index = 0; index < placementCount && valid; ++index) {
            const uint8_t* record = data.data() + placementsOffset
                + static_cast<size_t>(index) * kPlacementBytes;
            VitaEffectPlacement placement;
            placement.effectIndex = ReadU32(record);
            placement.flags = ReadU32(record + 4);
            for (size_t component = 0; component < 3; ++component) {
                placement.origin[component] = ReadFloat(record + 8 + component * 4);
                placement.angles[component] = ReadFloat(record + 20 + component * 4);
            }
            placement.delay = ReadFloat(record + 32);
            valid = placement.effectIndex < effects.size()
                && placement.flags != 0
                && (placement.flags & ~(VITA_EFFECT_ONESHOT
                                        | VITA_EFFECT_LOOP
                                        | VITA_EFFECT_EXPLODER)) == 0
                && Finite(placement.origin, 3)
                && Finite(placement.angles, 3)
                && std::isfinite(placement.delay);
            if (valid)
                placements.push_back(placement);
        }
        if (!valid)
            continue;

        std::vector<VitaLoopSoundPlacement> loopSounds;
        loopSounds.reserve(loopSoundCount);
        for (uint32_t index = 0; index < loopSoundCount && valid; ++index) {
            const uint8_t* record = data.data() + soundsOffset
                + static_cast<size_t>(index) * kLoopSoundBytes;
            VitaLoopSoundPlacement sound;
            const uint32_t aliasOffset = ReadU32(record);
            valid = ReadStringAt(data, stringBase, stringBytes, aliasOffset,
                                 kMaxSoundBytes, false, &sound.soundAlias);
            for (size_t component = 0; component < 3 && valid; ++component) {
                sound.origin[component] = ReadFloat(record + 4 + component * 4);
                sound.angles[component] = ReadFloat(record + 16 + component * 4);
            }
            valid = valid && Finite(sound.origin, 3) && Finite(sound.angles, 3);
            if (valid)
                loopSounds.push_back(std::move(sound));
        }
        if (!valid)
            continue;

        if (version >= kVersion2 && graphEffectCount != 0) {
            const size_t graphBase = stringBase + stringBytes;
            const size_t graphEffectsBase = graphBase;
            const size_t graphElementsBase = graphEffectsBase
                + static_cast<size_t>(graphEffectCount) * kGraphEffectBytes;
            const size_t graphVisualsBase = graphElementsBase
                + static_cast<size_t>(graphElementCount) * graphElementBytes;
            const size_t graphVelocityBase = graphVisualsBase
                + static_cast<size_t>(graphVisualCount) * kGraphVisualBytes;
            const size_t graphVisualSamplesBase = graphVelocityBase
                + static_cast<size_t>(graphVelocitySampleCount)
                    * kGraphVelocitySampleBytes;
            const size_t graphTrailVerticesBase = graphVisualSamplesBase
                + static_cast<size_t>(graphVisualSampleCount)
                    * kGraphVisualSampleBytes;
            const size_t graphTrailIndicesBase = graphTrailVerticesBase
                + static_cast<size_t>(graphTrailVertexCount)
                    * kGraphTrailVertexBytes;
            if (graphTrailIndicesBase
                    + static_cast<size_t>(graphTrailIndexCount)
                        * kGraphTrailIndexBytes
                != data.size())
                continue;

            std::vector<VitaEffectElement> graphElements;
            graphElements.reserve(graphElementCount);
            for (uint32_t index = 0; index < graphElementCount; ++index) {
                VitaEffectElement element;
                if (!ReadGraphElement(
                        data, graphElementsBase
                            + static_cast<size_t>(index) * graphElementBytes,
                        graphVisualsBase, graphVelocityBase,
                        graphVisualSamplesBase, graphTrailVerticesBase,
                        graphTrailIndicesBase, stringBase, stringBytes,
                        graphVisualCount, graphVelocitySampleCount,
                        graphVisualSampleCount, graphTrailVertexCount,
                        graphTrailIndexCount, version, &element)) {
                    valid = false;
                    break;
                }
                // Mark elements carry up to two authored material
                // references (world/character) for each source mark.  The
                // desktop visualCount is the mark count, not the flattened
                // material-reference count used by VFXA.  Preserve both
                // source values instead of rejecting a valid graph when a
                // mark has two material slots.
                // Light and spot-light elements carry authored light state,
                // not a material/model/sound visual pointer.  The desktop
                // layout still uses visualCount for their state sample
                // selection, so a valid light can have visualCount > 0 and
                // zero serialized visual names.  All other element types
                // retain the strict source-reference closure rule, with
                // marks allowing their two material slots per mark.
                const bool visualReferencesValid = element.elemType == 6u
                    || element.elemType == 7u
                    ? element.visualNames.empty()
                    : element.elemType == 9u
                    ? (version == kVersion3
                        ? element.visualNames.size()
                            == static_cast<size_t>(element.visualCount) * 2u
                        : element.visualNames.size()
                            <= static_cast<size_t>(element.visualCount) * 2u)
                    : element.visualCount == element.visualNames.size();
                if (!visualReferencesValid
                    || element.visualCount > 32
                    || element.velocitySamples.size()
                        != element.velIntervalCount + 1u
                    || (element.elemType == 10u
                        ? !element.visualSamples.empty()
                        : element.visualSamples.size()
                            != element.visStateIntervalCount + 1u)) {
                    valid = false;
                    break;
                }
                graphElements.push_back(std::move(element));
            }
            if (!valid)
                continue;

            std::vector<uint8_t> graphSeen(effectCount, 0);
            for (uint32_t index = 0; index < graphEffectCount; ++index) {
                const uint8_t* record = data.data() + graphEffectsBase
                    + static_cast<size_t>(index) * kGraphEffectBytes;
                const uint32_t effectIndex = ReadU32(record + 0);
                const uint32_t elementOffset = ReadU32(record + 24);
                const uint32_t elementCount = ReadU32(record + 28);
                if (effectIndex >= effects.size()
                    || graphSeen[effectIndex] != 0
                    || ReadU32(record + 32) != 0
                    || elementOffset > graphElements.size()
                    || elementCount > graphElements.size() - elementOffset
                    || elementCount > 32) {
                    valid = false;
                    break;
                }
                VitaEffectRecord& effect = effects[effectIndex];
                effect.graphPresent = true;
                effect.flags = ReadU32(record + 4);
                effect.msecLoopingLife = ReadI32(record + 8);
                effect.elemDefCountLooping = ReadU32(record + 12);
                effect.elemDefCountOneShot = ReadU32(record + 16);
                effect.elemDefCountEmission = ReadU32(record + 20);
                if (static_cast<uint64_t>(effect.elemDefCountLooping)
                        + effect.elemDefCountOneShot
                        + effect.elemDefCountEmission != elementCount) {
                    valid = false;
                    break;
                }
                effect.elements.assign(
                    graphElements.begin() + elementOffset,
                    graphElements.begin() + elementOffset + elementCount);
                graphSeen[effectIndex] = 1;
            }
            if (!valid)
                continue;
        }

        mapIndex_ = mapIndex;
        effects_ = std::move(effects);
        placements_ = std::move(placements);
        loopSounds_ = std::move(loopSounds);
        loaded_ = true;
        wireVersion_ = version;
        return true;
    }
    return false;
}

const VitaEffectRecord* VitaEffectBundle::Record(uint32_t index) const {
    return index < effects_.size() ? &effects_[index] : nullptr;
}

const VitaEffectPlacement* VitaEffectBundle::Placement(uint32_t index) const {
    return index < placements_.size() ? &placements_[index] : nullptr;
}

const VitaLoopSoundPlacement* VitaEffectBundle::LoopSound(uint32_t index) const {
    return index < loopSounds_.size() ? &loopSounds_[index] : nullptr;
}

const VitaEffectRecord* VitaEffectBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    for (const VitaEffectRecord& effect : effects_) {
        if (effect.name == name)
            return &effect;
    }
    return nullptr;
}

uint32_t VitaEffectBundle::FindReferenceIndex(const char* reference) const {
    if (!reference || !*reference)
        return UINT32_MAX;
    const std::string_view value(reference);
    const auto EqualFold = [](std::string_view left,
                              std::string_view right) {
        if (left.size() != right.size())
            return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const auto Lower = [](char character) {
                return character >= 'A' && character <= 'Z'
                    ? static_cast<char>(character - 'A' + 'a') : character;
            };
            if (Lower(left[index]) != Lower(right[index]))
                return false;
        }
        return true;
    };
    // Source FX runner references are paths. Keep path-first precedence so a
    // path that happens to share an alias with another definition remains
    // tied to the authored child effect.
    for (uint32_t index = 0; index < effects_.size(); ++index) {
        if (effects_[index].path == value)
            return index;
    }
    for (uint32_t index = 0; index < effects_.size(); ++index) {
        if (effects_[index].name == value)
            return index;
    }
    for (uint32_t index = 0; index < effects_.size(); ++index) {
        if (EqualFold(effects_[index].path, value))
            return index;
    }
    for (uint32_t index = 0; index < effects_.size(); ++index) {
        if (EqualFold(effects_[index].name, value))
            return index;
    }
    return UINT32_MAX;
}

VitaEffectVisualClass VitaEffectBundle::VisualClass(uint32_t index) const {
    const VitaEffectRecord* effect = Record(index);
    return effect ? ClassifyEffectPath(*effect)
                  : VitaEffectVisualClass::Other;
}

const VitaEffectRecord* VitaEffectBundle::WeatherEffect() const {
    for (const VitaEffectRecord& effect : effects_) {
        if (IsRainEffect(effect))
            return &effect;
    }
    return nullptr;
}

uint32_t VitaEffectBundle::WeatherPlacementCount() const {
    uint32_t count = 0;
    for (const VitaEffectPlacement& placement : placements_) {
        const VitaEffectRecord* effect = Record(placement.effectIndex);
        if (effect && IsRainEffect(*effect))
            ++count;
    }
    return count;
}

const VitaEffectPlacement* VitaEffectBundle::WeatherPlacement(
    uint32_t ordinal) const {
    uint32_t weatherOrdinal = 0;
    for (const VitaEffectPlacement& placement : placements_) {
        const VitaEffectRecord* effect = Record(placement.effectIndex);
        if (!effect || !IsRainEffect(*effect))
            continue;
        if (weatherOrdinal++ == ordinal)
            return &placement;
    }
    return nullptr;
}

uint32_t VitaEffectBundle::WeatherSeed() const {
    const VitaEffectRecord* weather = WeatherEffect();
    if (!weather)
        return 0;
    uint32_t hash = 2166136261u;
    for (const char character : weather->path)
        hash = HashMix(hash, static_cast<uint8_t>(character));
    for (uint32_t index = 0; index < placements_.size(); ++index) {
        const VitaEffectPlacement& placement = placements_[index];
        const VitaEffectRecord* effect = Record(placement.effectIndex);
        if (!effect || !IsRainEffect(*effect))
            continue;
        hash = HashMix(hash, index);
        hash = HashMix(hash, placement.flags);
        for (float value : placement.origin)
            hash = HashMix(hash, FloatBits(value));
        for (float value : placement.angles)
            hash = HashMix(hash, FloatBits(value));
        hash = HashMix(hash, FloatBits(placement.delay));
    }
    return hash == 0 ? 1u : hash;
}

} // namespace kisak::vita
