#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_font_bundle.h>
#include <cassert>
#include <cstring>
#include <limits>

using namespace kisak::vita;
using Bytes = std::vector<uint8_t>;
template<class T> static void Append(Bytes& output, const T& value) {
    const auto* data = reinterpret_cast<const uint8_t*>(&value);
    output.insert(output.end(), data, data + sizeof(value));
}
static Bytes Font(uint32_t version, bool glow = true) {
    Bytes output;
    const std::string name = "fonts/sourceFont", image = "source_atlas";
    const std::string material = version == 2 ? "fonts/source_material" : "";
    const std::string glowMaterial = version == 2 && glow ? "fonts/source_glow" : "";
    Append(output, uint32_t(0x46544956)); Append(output, version);
    Append(output, uint32_t(1));
    Append(output, uint64_t(name.size() + image.size() + material.size() + glowMaterial.size() + 48));
    Append(output, uint32_t(name.size())); Append(output, uint32_t(image.size()));
    Append(output, uint32_t(2)); Append(output, int32_t(16));
    if (version == 2) {
        Append(output, uint32_t(material.size())); Append(output, uint32_t(glowMaterial.size()));
    }
    for (const auto& value : {name, image, material, glowMaterial})
        output.insert(output.end(), value.begin(), value.end());
    for (uint16_t letter : {uint16_t(32), uint16_t(65)}) {
        Append(output, letter);
        for (uint8_t metric : {uint8_t(254), uint8_t(249), uint8_t(8),
                              uint8_t(letter == 32 ? 0 : 7), uint8_t(letter == 32 ? 0 : 14), uint8_t(0)})
            Append(output, metric);
        for (float coordinate : {0.0f, 0.25f, 0.5f, 0.75f}) Append(output, coordinate);
    }
    return output;
}
static bool Read(void* context, uint64_t offset, void* destination, size_t bytes) {
    const auto& input = *static_cast<const Bytes*>(context);
    if (offset > input.size() || bytes > input.size() - offset) return false;
    std::memcpy(destination, input.data() + offset, bytes);
    return true;
}
static bool Load(VitaFontBundle& fonts, const Bytes& payload) {
    KvapHeader header{};
    std::memcpy(header.magic, "KVAP", 4);
    header.major = 1; header.endian = 1; header.headerSize = sizeof(header);
    header.tocOffset = sizeof(header); header.tocCount = 1; header.packageId = 1;
    KvapChunk chunk{};
    chunk.type = static_cast<uint32_t>(KvapChunkType::Font); chunk.assetId = 1;
    chunk.offset = sizeof(header) + sizeof(chunk); chunk.alignment = 16;
    chunk.rawSize = chunk.storedSize = payload.size();
    chunk.crc32 = KvapCrc32(payload.data(), payload.size());
    Bytes pack;
    Append(pack, header); Append(pack, chunk);
    pack.insert(pack.end(), payload.begin(), payload.end());
    VitaAssetDatabase database;
    if (database.Mount(Read, &pack, pack.size()) != KvapError::Ok) return false;
    return fonts.Load(database);
}
int main() {
    VitaFontBundle fonts;
    const auto v1 = Font(1), v2 = Font(2);
    assert(Load(fonts, v1) && fonts.Version() == 1);
    assert(fonts.Find("fonts/sourceFont")->materialName.empty());
    for (int cycle = 0; cycle < 10; ++cycle) {
        assert(Load(fonts, v2) && fonts.Version() == 2 && fonts.Count() == 1);
        const auto* font = fonts.Find("fonts/sourceFont");
        assert(font && font->materialName == "fonts/source_material");
        assert(font->glowMaterialName == "fonts/source_glow" && font->imageName == "source_atlas");
        assert(font->glyphs.size() == 2 && font->pixelHeight == 16);
        assert(font->glyphs[0].letter == 32 && font->glyphs[0].pixelWidth == 0);
        assert(font->glyphs[1].x0 == -2 && font->glyphs[1].y0 == -7);
        assert(font->glyphs[1].s1 == 0.5f && font->glyphs[1].t1 == 0.75f);
#if KISAK_VITA_CPU_RUNTIME
        const auto* resident=font->glyphs.data();
        assert(Load(fonts,v2));
        assert(fonts.Find("fonts/sourceFont")->glyphs.data()==resident);
#endif
        fonts.Clear();
        assert(!fonts.Loaded() && fonts.Count() == 0 && fonts.Version() == 0);
    }
    for (size_t length = 0; length < v2.size(); ++length)
        assert(!Load(fonts, Bytes(v2.begin(), v2.begin() + length)));
    auto invalid = v2;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(invalid.data() + invalid.size() - 16, &nan, sizeof(nan));
    assert(!Load(fonts, invalid) && !fonts.Loaded() && fonts.Count() == 0);
    invalid = v2; invalid[44] = 0;
    assert(!Load(fonts, invalid));
    invalid = v2; invalid[4] = 99;
    assert(!Load(fonts, invalid));
    invalid = v2;
    invalid.insert(invalid.end(), v2.begin() + 20, v2.end());
    invalid[8] = 2;
    uint64_t dataBytes{}; std::memcpy(&dataBytes, v2.data() + 12, 8);
    dataBytes *= 2; std::memcpy(invalid.data() + 12, &dataBytes, 8);
    assert(!Load(fonts, invalid));
    assert(Load(fonts, Font(2, false)));
    assert(fonts.Find("fonts/sourceFont")->glowMaterialName.empty());
    assert(Load(fonts, v1));
    return 0;
}
