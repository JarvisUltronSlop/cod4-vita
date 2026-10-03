#include <vita/vita_shipment_entities.h>

#include <cstdlib>
#include <cmath>
#include <cstring>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x45544956; // "VITE"
constexpr uint32_t kVersion = 1;

uint32_t ReadU32(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool ReadQuoted(const std::string& block, const char* key, std::string* value) {
    const std::string keyToken = std::string("\"") + key + "\"";
    const size_t keyPosition = block.find(keyToken);
    if (keyPosition == std::string::npos)
        return false;
    const size_t firstQuote = block.find('"', keyPosition + keyToken.size());
    if (firstQuote == std::string::npos)
        return false;
    const size_t secondQuote = block.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos)
        return false;
    *value = block.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    return true;
}

bool ParseVector(const std::string& value, float output[3]) {
    char* end = nullptr;
    const char* cursor = value.c_str();
    for (int i = 0; i < 3; ++i) {
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        output[i] = std::strtof(cursor, &end);
        if (end == cursor)
            return false;
        cursor = end;
    }
    return true;
}

int32_t ParseInteger(const std::string& value) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() ? static_cast<int32_t>(parsed) : 0;
}

int HexDigit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool ParseGroundLighting(const std::string& value, uint32_t* packed,
                         uint8_t* primaryLightIndex) {
    if (!packed || !primaryLightIndex
        || (value.size() != 8 && value.size() != 10))
        return false;
    uint8_t channels[4]{};
    for (size_t channel = 0; channel < 4; ++channel) {
        const int high = HexDigit(value[channel * 2]);
        const int low = HexDigit(value[channel * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        channels[channel] = static_cast<uint8_t>((high << 4) | low);
    }
    uint8_t primary = 0;
    if (value.size() == 10) {
        const int high = HexDigit(value[8]);
        const int low = HexDigit(value[9]);
        if (high < 0 || low < 0)
            return false;
        primary = static_cast<uint8_t>((high << 4) | low);
    }
    *packed = static_cast<uint32_t>(channels[0])
        | (static_cast<uint32_t>(channels[1]) << 8)
        | (static_cast<uint32_t>(channels[2]) << 16)
        | (static_cast<uint32_t>(channels[3]) << 24);
    *primaryLightIndex = primary;
    return *packed != 0;
}

} // namespace

void ShipmentMapEntities::Clear() {
    text_.clear();
    records_.clear();
    dmSpawns_.clear();
    teamSpawns_.clear();
    loaded_ = false;
}

bool ShipmentMapEntities::Load(const VitaAssetDatabase& database, uint64_t assetId) {
    Clear();
    const KvapChunk* requested = assetId
        ? database.Find(VitaAssetType::World, assetId) : nullptr;
    if (assetId && !requested)
        return false;
    for (uint32_t chunkIndex = 0; chunkIndex < database.ChunkCount(); ++chunkIndex) {
        const KvapChunk* chunk = database.Chunk(chunkIndex);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::World)
            || (assetId && chunk != requested)
            || chunk->rawSize < 12 || chunk->rawSize > 2 * 1024 * 1024)
            continue;
        std::vector<uint8_t> bytes(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, bytes.data(), bytes.size()) != KvapError::Ok
            || ReadU32(bytes.data()) != kMagic || ReadU32(bytes.data() + 4) != kVersion)
            continue;
        const uint32_t textBytes = ReadU32(bytes.data() + 8);
        if (textBytes != bytes.size() - 12)
            continue;
        text_.assign(reinterpret_cast<const char*>(bytes.data() + 12), textBytes);
        break;
    }
    if (text_.empty())
        return false;

    size_t cursor = 0;
    while (cursor < text_.size()) {
        const size_t open = text_.find('{', cursor);
        if (open == std::string::npos)
            break;
        const size_t close = text_.find('}', open + 1);
        if (close == std::string::npos)
            break;
        const std::string block = text_.substr(open + 1, close - open - 1);
        ShipmentMapEntity entity{};
        if (ReadQuoted(block, "classname", &entity.classname)) {
            std::string value;
            if (ReadQuoted(block, "target", &value))
                entity.target = value;
            if (ReadQuoted(block, "targetname", &value))
                entity.targetname = value;
            if (ReadQuoted(block, "model", &value))
                entity.model = value;
            if (ReadQuoted(block, "script_linkname", &value))
                entity.scriptLinkName = value;
            if (ReadQuoted(block, "script_noteworthy", &value))
                entity.scriptNoteworthy = value;
            if (ReadQuoted(block, "spawnflags", &value))
                entity.spawnflags = ParseInteger(value);
            if (ReadQuoted(block, "dmg", &value))
                entity.damage = ParseInteger(value);
            if (ReadQuoted(block, "count", &value)) {
                entity.count = ParseInteger(value);
                entity.hasCount = true;
            }
            if (ReadQuoted(block, "health", &value)) {
                entity.health = ParseInteger(value);
                entity.hasHealth = true;
            }
            if (ReadQuoted(block, "origin", &value))
                entity.hasOrigin = ParseVector(value, entity.origin);
            if (ReadQuoted(block, "angles", &value))
                entity.hasAngles = ParseVector(value, entity.angles);
            if (ReadQuoted(block, "modelscale", &value)) {
                char* end = nullptr;
                entity.modelScale = std::strtof(value.c_str(), &end);
                entity.hasModelScale = end != value.c_str()
                    && std::isfinite(entity.modelScale)
                    && entity.modelScale > 0.0f
                    && entity.modelScale <= 64.0f;
                if (!entity.hasModelScale)
                    entity.modelScale = 1.0f;
            }
            if (ReadQuoted(block, "gndLt", &value))
                entity.hasGroundLighting = ParseGroundLighting(
                    value, &entity.groundLighting,
                    &entity.groundPrimaryLightIndex);
            if (ReadQuoted(block, "primaryLightIndex", &value)) {
                const int32_t parsed = ParseInteger(value);
                entity.hasPrimaryLightIndex = parsed >= 0 && parsed <= 255;
                if (entity.hasPrimaryLightIndex)
                    entity.primaryLightIndex = static_cast<uint8_t>(parsed);
            }
            // VITE v1 packages emitted before the explicit field used the
            // fifth gndLt byte for the same source instance selection.
            if (entity.hasGroundLighting) {
                entity.primaryLightIndex = entity.groundPrimaryLightIndex;
                entity.hasPrimaryLightIndex = true;
            }
            records_.push_back(entity);

            if (entity.hasOrigin) {
                ShipmentSpawnPoint spawn{};
                std::memcpy(spawn.origin, entity.origin, sizeof(spawn.origin));
                if (entity.hasAngles)
                    std::memcpy(spawn.angles, entity.angles, sizeof(spawn.angles));
                if (entity.classname == "mp_dm_spawn")
                    dmSpawns_.push_back(spawn);
                else if (entity.classname == "mp_tdm_spawn")
                    teamSpawns_.push_back(spawn);
            }
        }
        cursor = close + 1;
    }
    loaded_ = !dmSpawns_.empty() || !teamSpawns_.empty();
    return loaded_;
}

} // namespace kisak::vita
