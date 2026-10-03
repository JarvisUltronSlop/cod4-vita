#include <vita/vita_font_bundle.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x46544956; // "VITF"
constexpr uint32_t kMaxRecords = 64;
constexpr uint32_t kMaxStringBytes = 4096;
constexpr uint32_t kMaxGlyphs = 2048;
constexpr uint64_t kMaxAggregateBytes = 8ull * 1024ull * 1024ull;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kGlyphBytes = 24;

uint32_t ReadU32(const uint8_t* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

uint16_t ReadU16(const uint8_t* bytes) {
    uint16_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

int32_t ReadI32(const uint8_t* bytes) {
    int32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

float ReadFloat(const uint8_t* bytes) {
    float value = 0.0f;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool ReadString(const std::vector<uint8_t>& data, size_t* offset,
                uint32_t length, std::string* value, bool allowEmpty) {
    if (!offset || !value || (!allowEmpty && length == 0) || length > kMaxStringBytes
        || *offset > data.size() || length > data.size() - *offset)
        return false;
    if (std::memchr(data.data() + *offset, '\0', length))
        return false;
    value->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
    *offset += length;
    return true;
}

} // namespace

void VitaFontBundle::Clear() {
#if KISAK_VITA_CPU_RUNTIME
    catalogKey_={};
#endif
    fonts_.clear();
    loaded_ = false;
    version_ = 0;
}

bool VitaFontBundle::Load(const VitaAssetDatabase& database) {
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuCatalogKey key;key.Capture(database,VitaAssetType::Font);
    if(loaded_ && key==catalogKey_)return true;
#endif
    Clear();
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Font)
            || chunk->rawSize < kHeaderBytes || chunk->rawSize > kMaxAggregateBytes)
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(chunk->rawSize));
        if (database.Read(chunk, data.data(), data.size()) != KvapError::Ok
            || ReadU32(data.data()) != kMagic)
            continue;
        const uint32_t version = ReadU32(data.data() + 4);
        if (version != 1 && version != 2)
            continue;
        const size_t recordHeaderBytes = version == 2 ? 24 : 16;
        const uint32_t recordCount = ReadU32(data.data() + 8);
        uint64_t declaredBytes = 0;
        std::memcpy(&declaredBytes, data.data() + 12, sizeof(declaredBytes));
        if (recordCount == 0 || recordCount > kMaxRecords || declaredBytes > kMaxAggregateBytes)
            continue;
        size_t offset = kHeaderBytes;
        uint64_t aggregateBytes = 0;
        std::vector<VitaFontRecord> parsed;
        parsed.reserve(recordCount);
        bool valid = true;
        for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
            if (offset > data.size() || data.size() - offset < recordHeaderBytes) {
                valid = false;
                break;
            }
            const uint32_t nameBytes = ReadU32(data.data() + offset);
            const uint32_t imageBytes = ReadU32(data.data() + offset + 4);
            const uint32_t glyphCount = ReadU32(data.data() + offset + 8);
            const int32_t pixelHeight = ReadI32(data.data() + offset + 12);
            const uint32_t materialBytes = version == 2
                ? ReadU32(data.data() + offset + 16) : 0;
            const uint32_t glowBytes = version == 2
                ? ReadU32(data.data() + offset + 20) : 0;
            offset += recordHeaderBytes;
            VitaFontRecord font;
            font.pixelHeight = pixelHeight;
            if (pixelHeight <= 0 || pixelHeight > 256 || glyphCount == 0 || glyphCount > kMaxGlyphs
                || !ReadString(data, &offset, nameBytes, &font.name, false)
                || !ReadString(data, &offset, imageBytes, &font.imageName, true)
                || !ReadString(data, &offset, materialBytes, &font.materialName, version == 1)
                || !ReadString(data, &offset, glowBytes, &font.glowMaterialName, true)) {
                valid = false;
                break;
            }
            const uint64_t glyphBytes = static_cast<uint64_t>(glyphCount) * kGlyphBytes;
            if (glyphBytes > data.size() - offset) {
                valid = false;
                break;
            }
            font.glyphs.reserve(glyphCount);
            for (uint32_t glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex) {
                const uint8_t* glyphData = data.data() + offset;
                VitaFontGlyph glyph;
                glyph.letter = ReadU16(glyphData);
                glyph.x0 = static_cast<int8_t>(glyphData[2]);
                glyph.y0 = static_cast<int8_t>(glyphData[3]);
                glyph.dx = glyphData[4];
                glyph.pixelWidth = glyphData[5];
                glyph.pixelHeight = glyphData[6];
                glyph.s0 = ReadFloat(glyphData + 8);
                glyph.t0 = ReadFloat(glyphData + 12);
                glyph.s1 = ReadFloat(glyphData + 16);
                glyph.t1 = ReadFloat(glyphData + 20);
                // COD4 stores the space glyph as an advance-only record with
                // zero bitmap dimensions; all drawable glyphs must retain
                // nonzero metrics.
                if (glyph.dx == 0 || (glyph.letter != 32
                    && (glyph.pixelWidth == 0 || glyph.pixelHeight == 0))
                    || !std::isfinite(glyph.s0) || !std::isfinite(glyph.t0)
                    || !std::isfinite(glyph.s1) || !std::isfinite(glyph.t1)
                    || glyph.s0 < 0.0f || glyph.s0 > 1.0f || glyph.t0 < 0.0f || glyph.t0 > 1.0f
                    || glyph.s1 < 0.0f || glyph.s1 > 1.0f || glyph.t1 < 0.0f || glyph.t1 > 1.0f) {
                    valid = false;
                    break;
                }
                font.glyphs.push_back(glyph);
                offset += kGlyphBytes;
            }
            if (!valid)
                break;
            aggregateBytes += nameBytes + imageBytes + materialBytes + glowBytes + glyphBytes;
            if (aggregateBytes > kMaxAggregateBytes)
                valid = false;
            parsed.push_back(std::move(font));
        }
        if (!valid || offset != data.size() || aggregateBytes != declaredBytes)
            continue;
        std::sort(parsed.begin(), parsed.end(),
                  [](const VitaFontRecord& left, const VitaFontRecord& right) {
                      return left.name < right.name;
                  });
        if (std::adjacent_find(parsed.begin(), parsed.end(),
                [](const VitaFontRecord& left, const VitaFontRecord& right) {
                    return left.name == right.name;
                }) != parsed.end())
            continue;
        fonts_ = std::move(parsed);
        loaded_ = true;
#if KISAK_VITA_CPU_RUNTIME
        catalogKey_=key;
#endif
        version_ = version;
        return true;
    }
    return false;
}

const VitaFontRecord* VitaFontBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(fonts_.begin(), fonts_.end(),
                                    [name](const VitaFontRecord& font) {
                                        return font.name == name;
                                    });
    return found == fonts_.end() ? nullptr : &*found;
}

const VitaFontRecord* VitaFontBundle::Record(uint32_t index) const {
    return index < fonts_.size() ? &fonts_[index] : nullptr;
}

} // namespace kisak::vita
