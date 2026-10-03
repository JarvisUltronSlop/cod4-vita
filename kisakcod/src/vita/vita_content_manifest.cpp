#include <vita/vita_content_manifest.h>

#include <vita/vita_map_assets.h>

#include <array>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>

namespace kisak::vita {
namespace {

constexpr size_t kManifestCapacity = 16u * 1024u;

struct FileContext {
    FILE* file = nullptr;
};

bool ReadFile(void* context, uint64_t offset, void* destination, size_t size) {
    auto* source = static_cast<FileContext*>(context);
    if (!source || !source->file || offset > static_cast<uint64_t>(LONG_MAX))
        return false;
    return std::fseek(source->file, static_cast<long>(offset), SEEK_SET) == 0
        && std::fread(destination, 1, size, source->file) == size;
}

const char* FindValue(const char* json, size_t bytes, const char* key) {
    if (!json || !key)
        return nullptr;
    char token[72]{};
    const int tokenBytes = std::snprintf(token, sizeof(token), "\"%s\"", key);
    if (tokenBytes <= 0 || static_cast<size_t>(tokenBytes) >= sizeof(token))
        return nullptr;
    for (size_t offset = 0; offset + static_cast<size_t>(tokenBytes) < bytes;
         ++offset) {
        if (std::memcmp(json + offset, token, static_cast<size_t>(tokenBytes)) != 0)
            continue;
        const char* cursor = json + offset + tokenBytes;
        const char* end = json + bytes;
        while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
        if (cursor >= end || *cursor++ != ':')
            continue;
        while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
        return cursor < end ? cursor : nullptr;
    }
    return nullptr;
}

bool ParseBool(const char* json, size_t bytes, const char* key, bool* output) {
    const char* value = FindValue(json, bytes, key);
    if (!value || !output)
        return false;
    if (std::memcmp(value, "true", 4) == 0) {
        *output = true;
        return true;
    }
    if (std::memcmp(value, "false", 5) == 0) {
        *output = false;
        return true;
    }
    return false;
}

bool ParseUint64(const char* json, size_t bytes, const char* key,
                 uint64_t* output) {
    const char* value = FindValue(json, bytes, key);
    if (!value || !output)
        return false;
    const char* end = json + bytes;
    bool quoted = value < end && *value == '"';
    if (quoted)
        ++value;
    int base = 10;
    if (value + 2 <= end && value[0] == '0'
        && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value += 2;
    }
    uint64_t result = 0;
    bool found = false;
    for (; value < end; ++value) {
        const unsigned char character = static_cast<unsigned char>(*value);
        uint32_t digit = 0;
        if (character >= '0' && character <= '9')
            digit = character - '0';
        else if (base == 16 && character >= 'a' && character <= 'f')
            digit = character - 'a' + 10u;
        else if (base == 16 && character >= 'A' && character <= 'F')
            digit = character - 'A' + 10u;
        else
            break;
        if (result > (std::numeric_limits<uint64_t>::max() - digit)
                         / static_cast<uint64_t>(base))
            return false;
        result = result * static_cast<uint64_t>(base) + digit;
        found = true;
    }
    if (quoted && (value >= end || *value != '"'))
        return false;
    if (!found)
        return false;
    *output = result;
    return true;
}

bool ParseString(const char* json, size_t bytes, const char* key,
                 char* output, size_t capacity) {
    const char* value = FindValue(json, bytes, key);
    const char* end = json + bytes;
    if (!value || !output || capacity == 0 || value >= end || *value++ != '"')
        return false;
    size_t length = 0;
    while (value < end && *value != '"') {
        if (*value == '\\' || static_cast<unsigned char>(*value) < 0x20
            || length + 1 >= capacity)
            return false;
        output[length++] = *value++;
    }
    if (value >= end || *value != '"')
        return false;
    output[length] = '\0';
    return true;
}

VitaContentError ParseManifest(const VitaAssetDatabase& database,
                               const KvapChunk& chunk,
                               VitaContentRole expectedRole,
                               VitaContentManifest* output) {
    if (chunk.rawSize == 0 || chunk.rawSize >= kManifestCapacity)
        return VitaContentError::ManifestTooLarge;
    std::array<char, kManifestCapacity> json{};
    if (database.Read(&chunk, json.data(), json.size()) != KvapError::Ok)
        return VitaContentError::ManifestCorrupt;

    VitaContentManifest result{};
    uint64_t schema = 0;
    char role[16]{};
    if (!ParseUint64(json.data(), static_cast<size_t>(chunk.rawSize),
                     "schemaVersion", &schema)
        || schema < 1 || schema > kVitaContentSchemaVersion)
        return VitaContentError::SchemaMismatch;
    if (schema == 2) {
        char residency[32]{};
        if (!ParseString(json.data(), static_cast<size_t>(chunk.rawSize),
                         "textureResidency", residency, sizeof(residency))
            || std::strcmp(residency, "paged-world-v1") != 0)
            return VitaContentError::SchemaMismatch;
    }
    result.schemaVersion = static_cast<uint32_t>(schema);
    if (!ParseString(json.data(), static_cast<size_t>(chunk.rawSize),
                     "packageRole", role, sizeof(role)))
        return VitaContentError::ManifestCorrupt;
    result.role = std::strcmp(role, "common") == 0 ? VitaContentRole::Common
        : std::strcmp(role, "map") == 0 ? VitaContentRole::Map
        : VitaContentRole::Unknown;
    if (result.role != expectedRole)
        return VitaContentError::RoleMismatch;
    if (!ParseBool(json.data(), static_cast<size_t>(chunk.rawSize),
                   "certified", &result.certified))
        return VitaContentError::ManifestCorrupt;
    if (!result.certified)
        return VitaContentError::NotCertified;
    bool playable = false;
    char payloadPolicy[32]{};
    if (!ParseBool(json.data(), static_cast<size_t>(chunk.rawSize),
                   "playable", &playable) || !playable
        || !ParseString(json.data(), static_cast<size_t>(chunk.rawSize),
                        "payloadPolicy", payloadPolicy, sizeof(payloadPolicy))
        || std::strcmp(payloadPolicy, "runtime-ready-v1") != 0)
        return VitaContentError::NotRuntimeReady;
    if (!ParseUint64(json.data(), static_cast<size_t>(chunk.rawSize),
                     "predictedCpuResidentBytes",
                     &result.predictedCpuResidentBytes)
        || !ParseUint64(json.data(), static_cast<size_t>(chunk.rawSize),
                        "predictedGpuResidentBytes",
                        &result.predictedGpuResidentBytes))
        return VitaContentError::ManifestCorrupt;
    if (result.predictedCpuResidentBytes > kVitaNonGpuBudgetBytes
        || result.predictedGpuResidentBytes > kVitaGpuReserveBytes
        || result.predictedCpuResidentBytes + result.predictedGpuResidentBytes
            > kVitaWorkingBudgetBytes)
        return VitaContentError::BudgetExceeded;

    if (const KvapHeader* header = database.HeaderFor(&chunk)) {
        result.packageId = header->packageId;
        std::memcpy(result.sourceInstallHash, header->sourceHash,
                    sizeof(result.sourceInstallHash));
    } else {
        return VitaContentError::PackageRejected;
    }
    if (result.role == VitaContentRole::Map) {
        uint64_t mapIndex = UINT64_MAX;
        if (!ParseUint64(json.data(), static_cast<size_t>(chunk.rawSize),
                         "mapIndex", &mapIndex)
            || mapIndex >= kVitaMpMapCount
            || !VitaMapIsEnabled(static_cast<uint32_t>(mapIndex)))
            return VitaContentError::MapRejected;
        result.mapIndex = static_cast<uint32_t>(mapIndex);
        if (!ParseString(json.data(), static_cast<size_t>(chunk.rawSize),
                         "mapId", result.mapId, sizeof(result.mapId))
            || std::strcmp(result.mapId, kVitaMpMaps[result.mapIndex].name) != 0
            || !ParseUint64(json.data(), static_cast<size_t>(chunk.rawSize),
                            "commonPackageId", &result.commonPackageId))
            return VitaContentError::ManifestCorrupt;
    }
    *output = result;
    return VitaContentError::Ok;
}

} // namespace

VitaContentError VitaContentManifest_Load(const VitaAssetDatabase& database,
                                          VitaContentRole expectedRole,
                                          VitaContentManifest* output) {
    if (!database.Mounted() || !output || expectedRole == VitaContentRole::Unknown)
        return VitaContentError::InvalidArgument;
    VitaContentError lastError = VitaContentError::ManifestMissing;
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(KvapChunkType::Manifest))
            continue;
        const VitaContentError error = ParseManifest(database, *chunk,
                                                     expectedRole, output);
        if (error == VitaContentError::Ok)
            return error;
        lastError = error;
    }
    return lastError;
}

VitaContentError VitaContentManifest_ProbeFile(const char* path,
                                               VitaContentRole expectedRole,
                                               VitaContentManifest* output) {
    if (!path || !output)
        return VitaContentError::InvalidArgument;
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return VitaContentError::OpenFailed;
    VitaContentError result = VitaContentError::PackageRejected;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long end = std::ftell(file);
        if (end >= 0 && std::fseek(file, 0, SEEK_SET) == 0) {
            FileContext context{file};
            VitaAssetDatabase database;
            if (database.Mount(ReadFile, &context, static_cast<uint64_t>(end))
                == KvapError::Ok)
                result = VitaContentManifest_Load(database, expectedRole, output);
        }
    }
    std::fclose(file);
    return result;
}

VitaContentError VitaContentManifest_ValidateDependency(
    const VitaContentManifest& common, const VitaContentManifest& map) {
    if (common.role != VitaContentRole::Common || map.role != VitaContentRole::Map)
        return VitaContentError::RoleMismatch;
    if (!common.certified || !map.certified)
        return VitaContentError::NotCertified;
    if (map.schemaVersion != common.schemaVersion
        || map.commonPackageId == 0 || map.commonPackageId != common.packageId
        || std::memcmp(map.sourceInstallHash, common.sourceInstallHash,
                       sizeof(common.sourceInstallHash)) != 0)
        return VitaContentError::DependencyMismatch;
    const uint64_t cpu = common.predictedCpuResidentBytes
        + map.predictedCpuResidentBytes;
    const uint64_t gpu = common.predictedGpuResidentBytes
        + map.predictedGpuResidentBytes;
    if (cpu > kVitaNonGpuBudgetBytes || gpu > kVitaGpuReserveBytes
        || cpu + gpu > kVitaWorkingBudgetBytes)
        return VitaContentError::BudgetExceeded;
    return VitaContentError::Ok;
}

const char* VitaContentErrorName(VitaContentError error) {
    switch (error) {
    case VitaContentError::Ok: return "ok";
    case VitaContentError::InvalidArgument: return "invalid-argument";
    case VitaContentError::OpenFailed: return "open-failed";
    case VitaContentError::PackageRejected: return "package-rejected";
    case VitaContentError::ManifestMissing: return "manifest-missing";
    case VitaContentError::ManifestTooLarge: return "manifest-too-large";
    case VitaContentError::ManifestCorrupt: return "manifest-corrupt";
    case VitaContentError::SchemaMismatch: return "schema-mismatch";
    case VitaContentError::RoleMismatch: return "role-mismatch";
    case VitaContentError::NotCertified: return "not-certified";
    case VitaContentError::NotRuntimeReady: return "not-runtime-ready";
    case VitaContentError::MapRejected: return "map-rejected";
    case VitaContentError::BudgetExceeded: return "budget-exceeded";
    case VitaContentError::DependencyMismatch: return "dependency-mismatch";
    }
    return "unknown";
}

} // namespace kisak::vita
