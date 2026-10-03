#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_animation_bundle.h>
#include <vita/vita_animation_record.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <algorithm>

using namespace kisak::vita;
using Bytes = std::vector<uint8_t>;
static size_t allocationLimit = SIZE_MAX, largestAllocation = 0, allocations = 0;
static size_t failAt = SIZE_MAX;
void* operator new(size_t size) {
    largestAllocation = std::max(largestAllocation, size);
    if (++allocations == failAt || size > allocationLimit) throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

template<class T> static void Put(Bytes& b, size_t offset, const T& value) {
    assert(offset + sizeof(value) <= b.size());
    std::memcpy(b.data() + offset, &value, sizeof(value));
}
static Bytes Animation(uint32_t version) {
    const uint32_t width = version == 1 ? 182 : version == 2 ? 162 : 194;
    Bytes b(16 + width);
    Put(b, 0, uint32_t(0x51544956)); Put(b, 4, version);
    Put(b, 8, uint32_t(1)); Put(b, 12, width);
    std::memcpy(b.data() + 16, "test_anim", 10);
    Put(b, 16 + 128, uint16_t(12));
    const size_t rate = version == 1 ? 174 : version == 2 ? 154 : 186;
    Put(b, 16 + rate, 30.0f); Put(b, 16 + rate + 4, 2.5f);
    if (version == 3) {
        // Native bytes exercise copying into a decoded allocation.
        Put(b, 16 + 146, uint16_t(4)); Put(b, 16 + 170, uint32_t(4));
        b.insert(b.end(), {1, 2, 3, 4});
    }
    return b;
}
static Bytes Pack(const Bytes& payload) {
    KvapHeader header{};
    std::memcpy(header.magic, "KVAP", 4);
    header.major = 1; header.endian = 1; header.headerSize = sizeof(header);
    header.tocOffset = sizeof(header); header.tocCount = 1; header.packageId = 1;
    KvapChunk chunk{};
    chunk.type = static_cast<uint32_t>(KvapChunkType::Animation); chunk.assetId = 1;
    chunk.offset = sizeof(header) + sizeof(chunk); chunk.alignment = 16;
    chunk.rawSize = chunk.storedSize = payload.size();
    chunk.crc32 = KvapCrc32(payload.data(), payload.size());
    Bytes output(static_cast<size_t>(chunk.offset));
    Put(output, 0, header); Put(output, sizeof(header), chunk);
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}
struct Reader { const Bytes* bytes; size_t maxRead = 0; uint64_t failOffset = UINT64_MAX; };
static bool Read(void* context, uint64_t offset, void* destination, size_t size) {
    auto& reader = *static_cast<Reader*>(context);
    reader.maxRead = std::max(reader.maxRead, size);
    if (offset >= reader.failOffset || offset > reader.bytes->size()
        || size > reader.bytes->size() - offset) return false;
    std::memcpy(destination, reader.bytes->data() + offset, size);
    return true;
}
static bool Load(VitaAnimationBundle& bundle, const Bytes& payload) {
    auto pack = Pack(payload); Reader reader{&pack}; VitaAssetDatabase db;
    assert(db.Mount(Read, &reader, pack.size()) == KvapError::Ok);
    return bundle.Load(db);
}

// Hash each decoded field, never object padding, addresses, or vector capacity.
static uint64_t fingerprint = 14695981039346656037ull;
static void HashBytes(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { fingerprint ^= bytes[i]; fingerprint *= 1099511628211ull; }
}
template<class T> static void Hash(const T& v) { HashBytes(&v, sizeof(v)); }
static void Hash(const std::string& v) { Hash(uint64_t(v.size())); HashBytes(v.data(), v.size()); }
template<class T> static void Hash(const std::vector<T>& v) {
    Hash(uint64_t(v.size())); for (const auto& item : v) Hash(item);
}
static uint64_t Fingerprint(const VitaAnimationBundle& bundle) {
    fingerprint = 14695981039346656037ull;
    Hash(bundle.WireVersion()); Hash(bundle.Count());
    for (const auto& c : bundle.Records()) {
        Hash(c.name); Hash(c.numFrames); Hash(c.loop); Hash(c.delta); Hash(c.isDefault);
        Hash(c.sourceCompressed); Hash(c.boneCount); Hash(c.notifyCount); Hash(c.assetType);
        Hash(c.dataByteCount); Hash(c.dataShortCount); Hash(c.dataIntCount);
        Hash(c.randomDataByteCount); Hash(c.randomDataIntCount); Hash(c.randomDataShortCount);
        Hash(c.indexCount); Hash(c.framerate); Hash(c.frequency); Hash(c.boneNames);
        Hash(c.dataByte); Hash(c.dataShort); Hash(c.dataInt); Hash(c.randomDataByte);
        Hash(c.randomDataShort); Hash(c.randomDataInt); Hash(c.indices);
        const auto& d = c.deltaPart;
        Hash(d.present); Hash(d.hasTrans); Hash(d.hasQuat); Hash(d.transSmall);
        Hash(d.transSize); Hash(d.quatSize); Hash(d.transBase); Hash(d.transRange);
        Hash(d.quatFrame0); Hash(d.transFrames); Hash(d.transIndices); Hash(d.quatFrames); Hash(d.quatIndices);
        Hash(uint64_t(c.tracks.size()));
        for (const auto& t : c.tracks) {
            Hash(t.modelName); Hash(t.boneCount); Hash(t.frameCount); Hash(uint64_t(t.samples.size()));
            for (const auto& s : t.samples) { Hash(s.quat); Hash(s.trans); Hash(s.transWeight); }
        }
        Hash(uint64_t(c.notifies.size()));
        for (const auto& n : c.notifies) { Hash(n.name); Hash(n.time); }
    }
    return fingerprint;
}
struct FileReader { FILE* file; size_t maxRead = 0; };
static bool ReadFile(void* context, uint64_t offset, void* destination, size_t size) {
    auto& reader = *static_cast<FileReader*>(context);
    reader.maxRead = std::max(reader.maxRead, size);
    return std::fseek(reader.file, static_cast<long>(offset), SEEK_SET) == 0
        && std::fread(destination, 1, size, reader.file) == size;
}
int main(int argc, char** argv) {
    if (argc > 1) {
        FileReader reader{std::fopen(argv[1], "rb")}; assert(reader.file);
        assert(std::fseek(reader.file, 0, SEEK_END) == 0);
        const auto size = std::ftell(reader.file); assert(size > 0);
        VitaAssetDatabase db; assert(db.Mount(ReadFile, &reader, size) == KvapError::Ok);
        uint64_t expected = 0;
#ifndef ANIMATION_REFERENCE_LOADER
        allocationLimit = 1024 * 1024;
#endif
        largestAllocation = 0;
        for (int cycle = 0; cycle < 10; ++cycle) {
            VitaAnimationBundle bundle;
            assert(bundle.Load(db) && bundle.SourceComplete());
            const auto actual = Fingerprint(bundle);
            if (cycle == 0) expected = actual;
            assert(actual == expected);
            if (cycle == 9)
                std::printf("clips=%u notifies=%u decodedFingerprint=%016llx largestAllocation=%zu maxRead=%zu cycles=10\n",
                    bundle.Count(), bundle.NotifyCount(), static_cast<unsigned long long>(actual), largestAllocation, reader.maxRead);
        }
        allocationLimit = SIZE_MAX; std::fclose(reader.file); return 0;
    }
#ifndef ANIMATION_REFERENCE_LOADER
    assert(KvapCrc32("123456789", 9) == 0xcbf43926u);
    auto crc = KvapCrc32Update(0xffffffffu, "1234", 4);
    assert(~KvapCrc32Update(crc, "56789", 5) == 0xcbf43926u);
    VitaAnimationBundle bundle;
    for (uint32_t version : {1u, 2u, 3u}) {
        const auto data = Animation(version);
        assert(Load(bundle, data) && bundle.WireVersion() == version);
        assert(bundle.Find("test_anim")->numFrames == 12);
        if (version == 3) assert(bundle.Find("test_anim")->dataByte == Bytes({1, 2, 3, 4}));
        for (size_t length = 0; length < data.size(); ++length)
            assert(!Load(bundle, Bytes(data.begin(), data.begin() + length)) && bundle.Count() == 0);
    }
    auto data = Animation(3); Put(data, 16 + 170, uint32_t(0xffffffff));
    Put(data, 16 + 178, uint32_t(0xffffffff)); assert(!Load(bundle, data));
    data = Animation(3); data.push_back(0); assert(!Load(bundle, data));
    data = Animation(3); Put(data, 8, uint32_t(2)); assert(!Load(bundle, data));
    auto pack = Pack(Animation(3)); Reader reader{&pack}; VitaAssetDatabase db;
    assert(db.Mount(Read, &reader, pack.size()) == KvapError::Ok);
    pack.back() ^= 1; assert(!bundle.Load(db) && bundle.Count() == 0); pack.back() ^= 1;
    reader.failOffset = 128 + 16 + 194; assert(!bundle.Load(db)); reader.failOffset = UINT64_MAX;
    allocations = 0; assert(bundle.Load(db)); const auto allocationCount = allocations;
    // Fail every allocation site, including clip storage and decoded arrays.
    for (size_t site = 1; site <= allocationCount; ++site) {
        allocations = 0; failAt = site;
        assert(!bundle.Load(db) && !bundle.Loaded() && bundle.Count() == 0);
        assert(std::strcmp(bundle.LoadError(), "animation allocation failed") == 0);
        failAt = SIZE_MAX;
        assert(bundle.Load(db));
    }
    std::printf("animation versions, bounds, CRC, read failure, %zu allocation failure sites passed\n", allocationCount);
#endif
    return 0;
}
