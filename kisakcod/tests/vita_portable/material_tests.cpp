#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_material_bundle.h>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <cstdio>
using namespace kisak::vita;
using Bytes = std::vector<uint8_t>;
template<class T> static void Append(Bytes& out, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}
static bool Read(void* context, uint64_t offset, void* destination, size_t bytes) {
    const auto& input = *static_cast<const Bytes*>(context);
    if (offset > input.size() || bytes > input.size() - offset) return false;
    std::memcpy(destination, input.data() + offset, bytes); return true;
}
static bool Load(VitaMaterialBundle& materials, const Bytes& payload) {
    KvapHeader header{};
    std::memcpy(header.magic, "KVAP", 4);
    header.major = 1; header.endian = 1; header.headerSize = sizeof(header);
    header.tocOffset = sizeof(header); header.tocCount = 1; header.packageId = 1;
    KvapChunk chunk{};
    chunk.type = static_cast<uint32_t>(KvapChunkType::Material); chunk.assetId = 1;
    chunk.offset = sizeof(header) + sizeof(chunk); chunk.alignment = 16;
    chunk.rawSize = chunk.storedSize = payload.size();
    chunk.crc32 = KvapCrc32(payload.data(), payload.size());
    Bytes pack; Append(pack, header); Append(pack, chunk);
    pack.insert(pack.end(), payload.begin(), payload.end());
    VitaAssetDatabase database;
    return database.Mount(Read, &pack, pack.size()) == KvapError::Ok && materials.Load(database);
}
static Bytes Material(uint32_t version) {
    const std::string strings[5]{"fonts/test", "atlas", "", "", "2d"};
    Bytes out; Append(out, uint32_t(0x4C544956)); Append(out, version); Append(out, uint32_t(1));
    uint64_t stringBytes = 0;
    for (const auto& s : strings) stringBytes += s.size();
    Append(out, stringBytes);
    for (const auto& s : strings) Append(out, static_cast<uint32_t>(s.size()));
    Append(out, uint32_t(0x10));
    for (uint8_t b : {0, 43, 0, 1, 0x62, 0, 0, 3, 0}) Append(out, b);
    Append(out, uint32_t(0x08004800)); Append(out, uint32_t(5)); Append(out, uint8_t(1));
    if (version == 5) {
        Append(out, uint32_t(0x18004965)); Append(out, uint32_t(2));
        Append(out, uint8_t(1)); Append(out, uint8_t(0));
    }
    for (const auto& s : strings) out.insert(out.end(), s.begin(), s.end());
    return out;
}
static Bytes NamedMaterials(const std::vector<std::string>& names) {
    Bytes out; Append(out, uint32_t(0x4C544956)); Append(out, uint32_t(1));
    Append(out, uint32_t(names.size()));
    uint64_t size=0;
    for (const auto& name:names) size+=name.size()+4;
    Append(out,size);
    for (uint32_t i=0;i<names.size();++i) {
        Append(out,uint32_t(names[i].size())); Append(out,uint32_t(4));
        out.insert(out.end(),names[i].begin(),names[i].end());
        Append(out,i); // Different payloads expose duplicate-selection changes.
    }
    return out;
}
static const VitaMaterialRecord* LinearFind(const VitaMaterialBundle& materials, const char* key) {
    if (!key) return nullptr;
    for (uint32_t i=0;i<materials.Count();++i)
        if (materials.Record(i)->name==key) return materials.Record(i);
    return nullptr;
}
static void CheckLookup(const VitaMaterialBundle& materials,const char* key) {
    assert(materials.Find(key)==LinearFind(materials,key));
}
int main() {
    VitaMaterialBundle materials;
    const auto v4 = Material(4), v5 = Material(5);
    assert(Load(materials, v4));
    assert(!materials.Record(0)->unlitStateValid && materials.Record(0)->colorSemantic == 2);
    for (int cycle = 0; cycle < 10; ++cycle) {
        assert(Load(materials, v5) && materials.Count() == 1);
        const auto* m = materials.Find("fonts/test");
        assert(m && m->colorMap == "atlas" && m->colorSemantic == 0 && m->colorSamplerState == 0x62);
        assert(m->litStateValid == 1 && m->litStateBits0 == 0x08004800 && m->litStateBits1 == 5);
        assert(m->unlitStateValid == 1 && m->unlitStateBits0 == 0x18004965 && m->unlitStateBits1 == 2);
        materials.Clear(); assert(!materials.Loaded() && materials.Count() == 0);
    }
    for (size_t n = 0; n < v5.size(); ++n)
        assert(!Load(materials, Bytes(v5.begin(), v5.begin() + n)));
    auto invalid = v5; invalid[71] = 11; // water union cannot become a 2D image
    assert(!Load(materials, invalid) && !materials.Loaded());
    invalid = v5; invalid.push_back(0); assert(!Load(materials, invalid));
    invalid = v5; invalid[4] = 99; assert(!Load(materials, invalid));
    assert(Load(materials, v4));
    // Exercise the real loader and exact returned pointer, including first
    // duplicate, prefix, case, non-ASCII and embedded NUL names. Load's record
    // order is the oracle; Find must neither reorder nor retain old records.
    std::vector<std::string> names{"a","A","aa","a/","a0","duplicate","duplicate",
        std::string("a\0hidden",8),std::string(1,char(0xff)),std::string(1,char(0x80))};
    while(names.size()<4096) names.push_back("material/"+std::to_string(names.size()%3001));
    std::reverse(names.begin(),names.end());
    assert(Load(materials,NamedMaterials(names)) && materials.Count()==4096);
    for(uint32_t i=0;i<materials.Count();++i) CheckLookup(materials,materials.Record(i)->name.c_str());
    for(int i=0;i<512;++i) CheckLookup(materials,("missing/"+std::to_string(i)).c_str());
    CheckLookup(materials,nullptr); CheckLookup(materials,""); CheckLookup(materials,"a");
    for(int cycle=0;cycle<64;++cycle) {
        names={"map/"+std::to_string(cycle),"shared","shared","common"};
        assert(Load(materials,NamedMaterials(names)));
        for(const auto& key:names) CheckLookup(materials,key.c_str());
        CheckLookup(materials,"material/100");
        materials.Clear(); CheckLookup(materials,"shared");
        assert(Load(materials,NamedMaterials(names)));
        assert(!Load(materials,Bytes{0}) && materials.Count()==0);
        CheckLookup(materials,"shared");
    }
    std::puts("Material lookup parity: 4096-record bundle, duplicate/prefix/byte keys, missing/null and 64 reload/failure cycles");
}
