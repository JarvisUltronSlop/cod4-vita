#include <vita/vita_sound_bundle.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace kisak::vita;
static void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
template<class T> static void Append(std::vector<uint8_t>& out, const T& item) {
    const auto* at = reinterpret_cast<const uint8_t*>(&item);
    out.insert(out.end(), at, at + sizeof(item));
}
static std::vector<uint8_t> Pcm(const char* name) {
    std::vector<uint8_t> out;
    Append(out, uint32_t(0x53544956)); Append(out, uint32_t(1)); Append(out, uint32_t(1)); Append(out, uint64_t(8));
    for (uint32_t field : {uint32_t(std::strlen(name)), 8u, 48000u, 16u, 1u, 4u, 1u}) Append(out, field);
    out.insert(out.end(), name, name + std::strlen(name));
    for (int16_t sample : {int16_t(1000), int16_t(-2000), int16_t(32767), int16_t(-32768)}) Append(out, sample);
    return out;
}
static std::vector<uint8_t> Aliases() {
    std::vector<uint8_t> out;
    Append(out, kVitaSoundAliasMagic); Append(out, kVitaSoundAliasVersion);
    Append(out, uint32_t(2)); Append(out, uint64_t(2 * sizeof(VitaSoundAliasRecord)));
    for (const char* path : {"test/one.wav", "test/two.wav"}) {
        VitaSoundAliasRecord alias{};
        std::strcpy(alias.aliasName, "authored_alias"); std::strcpy(alias.soundFile, path);
        alias.soundType = alias.sourceFileExists = 1;
        alias.volMin = 0.25f; alias.volMax = 0.75f;
        alias.pitchMin = alias.pitchMax = 1;
        alias.distMin = 16; alias.distMax = 1024; alias.startDelay = 35;
        Append(out, alias);
    }
    return out;
}
static std::vector<uint8_t> Pack(const std::vector<std::vector<uint8_t>>& chunks) {
    KvapHeader header{};
    std::memcpy(header.magic, "KVAP", 4);
    header.major = 1; header.endian = 1; header.headerSize = sizeof(header);
    header.tocOffset = sizeof(header); header.tocCount = static_cast<uint32_t>(chunks.size()); header.packageId = 1;
    std::vector<uint8_t> out(sizeof(header) + chunks.size() * sizeof(KvapChunk));
    std::memcpy(out.data(), &header, sizeof(header));
    for (size_t index = 0; index < chunks.size(); ++index) {
        out.resize((out.size() + 31) & ~size_t(31));
        KvapChunk chunk{};
        chunk.type = static_cast<uint32_t>(KvapChunkType::Sound); chunk.assetId = index + 1;
        chunk.offset = out.size(); chunk.rawSize = chunk.storedSize = chunks[index].size(); chunk.alignment = 32;
        chunk.crc32 = KvapCrc32(chunks[index].data(), chunks[index].size());
        std::memcpy(out.data() + sizeof(header) + index * sizeof(chunk), &chunk, sizeof(chunk));
        out.insert(out.end(), chunks[index].begin(), chunks[index].end());
    }
    return out;
}
static void Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Check(bool(file), "fixture write failed");
}
int main() {
    const auto directory = std::filesystem::current_path() / "sound-bundle-fixture";
    std::filesystem::create_directories(directory);
    const auto common = directory / "common.kvap";
    const auto map = directory / "map.kvap";
    const auto broken = directory / "broken.kvap";
    Write(common, Pack({Pcm("test/one.wav")}));
    const auto original = Pack({Pcm("test/two.wav"), Aliases()});
    Write(map, original);
    VitaSoundBundle sounds;
    for (int cycle = 0; cycle < 10; ++cycle) {
        Check(sounds.LoadPackage(map.string().c_str(), common.string().c_str()), "owned map/common mount failed");
        Check(sounds.Count() == 2 && sounds.AliasCount() == 1 && sounds.PcmBytes() == 0, "metadata must stay separate from prepared PCM");
        std::vector<VitaSoundAliasRecord> aliases;
        Check(sounds.ReadAlias("authored_alias", &aliases) && aliases.size() == 2, "source variants missing");
        Check(aliases[0].startDelay == 35 && aliases[1].volMax == 0.75f
            && !std::strcmp(aliases[0].soundFile, "test/one.wav"), "source alias values/order changed");
        const auto* pcm = sounds.PreparePcm("TEST\\ONE.WAV");
        Check(pcm && pcm->bytes.size() == 8 && sounds.PcmBytes() == 8, "deferred read lost its backing file or normalized path");
        int16_t last{}; std::memcpy(&last, pcm->bytes.data() + 6, 2);
        Check(last == -32768, "PCM sample bytes changed");
        sounds.FreezePreparation();
        Check(sounds.PreparePcm("test/one.wav") == pcm, "prepared PCM unavailable after freeze");
        Check(!sounds.PreparePcm("test/two.wav") && !sounds.ReadAlias("authored_alias", &aliases), "gameplay performed forbidden pack preparation");
        sounds.Clear();
        Check(!sounds.Loaded() && sounds.Count() == 0 && sounds.AliasCount() == 0, "map teardown retained sound state");
    }
    auto corrupt = original; corrupt.back() ^= 1;
    Write(broken, corrupt);
    Check(!sounds.LoadPackage(broken.string().c_str(), common.string().c_str()), "corrupt alias CRC accepted");
    Check(!sounds.Loaded() && sounds.Count() == 0, "failed load leaked partial sound state");
    Check(!sounds.LoadPackage(map.string().c_str(), "missing-common.kvap"), "missing dependency accepted");
    Check(sounds.LoadPackage(map.string().c_str(), common.string().c_str()), "failed load prevented recovery");
    sounds.Clear();
    Check(std::filesystem::remove(common) && std::filesystem::remove(map) && std::filesystem::remove(broken), "owned pack handles were not closed");
    std::filesystem::remove(directory);
    return 0;
}
