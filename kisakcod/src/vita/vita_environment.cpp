#include <vita/vita_environment.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x564E4556; // "VENV"
constexpr uint32_t kVersion1 = 1;
constexpr uint32_t kVersion2 = 2;
constexpr uint32_t kVersion3 = 3;
constexpr size_t kHeaderBytesV1 = 116;
constexpr size_t kHeaderBytesV2 = 152;
constexpr size_t kHeaderBytesV3 = 156;
constexpr uint32_t kMaxStringBytes = 256;

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

bool Finite(const float* values, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index]))
            return false;
    }
    return true;
}

} // namespace

bool LoadVitaEnvironment(const VitaAssetDatabase& database, uint64_t assetId,
                         VitaEnvironmentState* output) {
    if (!output)
        return false;
    *output = {};
    const KvapChunk* chunk = database.Find(VitaAssetType::Environment, assetId);
    if (!chunk || chunk->rawSize < kHeaderBytesV1 || chunk->rawSize > 64 * 1024)
        return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(chunk->rawSize));
    if (database.Read(chunk, bytes.data(), bytes.size()) != KvapError::Ok)
        return false;
    if (ReadU32(bytes.data()) != kMagic)
        return false;
    const uint32_t version = ReadU32(bytes.data() + 4);
    const size_t headerBytes = version == kVersion3 ? kHeaderBytesV3
                            : version == kVersion2 ? kHeaderBytesV2
                            : version == kVersion1 ? kHeaderBytesV1 : 0;
    if (headerBytes == 0 || bytes.size() < headerBytes)
        return false;

    output->mapIndex = ReadU32(bytes.data() + 8);
    output->flags = ReadU32(bytes.data() + 12);
    for (size_t component = 0; component < 3; ++component)
        output->fogColor[component] = ReadFloat(bytes.data() + 16 + component * 4);
    output->fogStart = ReadFloat(bytes.data() + 28);
    output->fogDensity = ReadFloat(bytes.data() + 32);
    output->fogHalfway = ReadFloat(bytes.data() + 36);
    output->fogTransition = ReadFloat(bytes.data() + 40);
    for (size_t component = 0; component < 3; ++component)
        output->filmLightTint[component] = ReadFloat(bytes.data() + 44 + component * 4);
    for (size_t component = 0; component < 3; ++component)
        output->filmDarkTint[component] = ReadFloat(bytes.data() + 56 + component * 4);
    output->filmContrast = ReadFloat(bytes.data() + 68);
    output->filmBrightness = ReadFloat(bytes.data() + 72);
    output->filmDesaturation = ReadFloat(bytes.data() + 76);
    output->filmInvert = ReadFloat(bytes.data() + 80);
    output->rainRate = ReadFloat(bytes.data() + 84);
    output->rainSlant = ReadFloat(bytes.data() + 88);
    output->rainCoverage = ReadFloat(bytes.data() + 92);
    output->wetness = ReadFloat(bytes.data() + 96);
    if (version >= kVersion2) {
        const float glowEnabled = ReadFloat(bytes.data() + 116);
        output->glowEnabled = glowEnabled > 0.5f;
        output->glowRadius0 = ReadFloat(bytes.data() + 120);
        output->glowRadius1 = ReadFloat(bytes.data() + 124);
        output->glowBloomCutoff = ReadFloat(bytes.data() + 128);
        output->glowBloomDesaturation = ReadFloat(bytes.data() + 132);
        output->glowBloomIntensity0 = ReadFloat(bytes.data() + 136);
        output->glowBloomIntensity1 = ReadFloat(bytes.data() + 140);
        output->glowSkyBleedIntensity0 = ReadFloat(bytes.data() + 144);
        output->glowSkyBleedIntensity1 = ReadFloat(bytes.data() + 148);
    }
    output->specularColorScale = version >= kVersion3
        ? ReadFloat(bytes.data() + 152) : 1.0f;
    const uint32_t visionBytes = ReadU32(bytes.data() + 100);
    const uint32_t weatherBytes = ReadU32(bytes.data() + 104);
    output->effectCount = ReadU32(bytes.data() + 108);
    if (visionBytes > kMaxStringBytes || weatherBytes > kMaxStringBytes
        || headerBytes + static_cast<size_t>(visionBytes) + weatherBytes > bytes.size()) {
        *output = {};
        return false;
    }
    size_t offset = headerBytes;
    output->visionName.assign(reinterpret_cast<const char*>(bytes.data() + offset),
                              visionBytes);
    offset += visionBytes;
    output->weatherEffect.assign(reinterpret_cast<const char*>(bytes.data() + offset),
                                 weatherBytes);

    const bool finite = Finite(output->fogColor, 3)
        && Finite(output->filmLightTint, 3) && Finite(output->filmDarkTint, 3)
        && std::isfinite(output->fogStart) && std::isfinite(output->fogDensity)
        && std::isfinite(output->fogHalfway) && std::isfinite(output->fogTransition)
        && std::isfinite(output->filmContrast) && std::isfinite(output->filmBrightness)
        && std::isfinite(output->filmDesaturation) && std::isfinite(output->filmInvert)
        && std::isfinite(output->rainRate) && std::isfinite(output->rainSlant)
        && std::isfinite(output->rainCoverage) && std::isfinite(output->wetness)
        && std::isfinite(output->glowRadius0) && std::isfinite(output->glowRadius1)
        && std::isfinite(output->glowBloomCutoff)
        && std::isfinite(output->glowBloomDesaturation)
        && std::isfinite(output->glowBloomIntensity0)
        && std::isfinite(output->glowBloomIntensity1)
        && std::isfinite(output->glowSkyBleedIntensity0)
        && std::isfinite(output->glowSkyBleedIntensity1)
        && std::isfinite(output->specularColorScale);
    if (!finite) {
        *output = {};
        return false;
    }
    output->fogStart = std::max(0.0f, output->fogStart);
    output->fogDensity = std::max(0.0f, output->fogDensity);
    output->rainRate = std::max(0.0f, output->rainRate);
    output->rainCoverage = std::clamp(output->rainCoverage, 0.0f, 1.0f);
    output->wetness = std::clamp(output->wetness, 0.0f, 1.0f);
    output->glowRadius0 = std::clamp(output->glowRadius0, 0.0f, 128.0f);
    output->glowRadius1 = std::clamp(output->glowRadius1, 0.0f, 128.0f);
    output->glowBloomCutoff = std::clamp(output->glowBloomCutoff, 0.0f, 1.0f);
    output->glowBloomDesaturation = std::clamp(
        output->glowBloomDesaturation, 0.0f, 1.0f);
    output->glowBloomIntensity0 = std::clamp(
        output->glowBloomIntensity0, 0.0f, 8.0f);
    output->glowBloomIntensity1 = std::clamp(
        output->glowBloomIntensity1, 0.0f, 8.0f);
    output->glowSkyBleedIntensity0 = std::clamp(
        output->glowSkyBleedIntensity0, 0.0f, 8.0f);
    output->glowSkyBleedIntensity1 = std::clamp(
        output->glowSkyBleedIntensity1, 0.0f, 8.0f);
    output->specularColorScale = std::clamp(
        output->specularColorScale, 0.0f, 100.0f);
    output->glowEnabled = output->glowEnabled
        && (output->flags & VITA_ENV_HAS_GLOW) != 0
        && output->glowRadius0 > 0.0f
        && output->glowBloomIntensity0 > 0.0f;
    output->valid = true;
    return true;
}

} // namespace kisak::vita
