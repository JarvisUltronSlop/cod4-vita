#include <vita/vita_sound_bundle.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <climits>
#include <string_view>
#include <utility>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x53544956; // "VITS"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxRecords = 2048;
constexpr uint32_t kMaxNameBytes = 4096;
constexpr uint32_t kMaxSampleBytes = 8u * 1024u * 1024u;
// Shipment's map set includes the decoded pool plus weapon reload aliases
// captured directly from the desktop sound-alias records. The exporter caps
// PCM at 80 MiB; allow the bounded VITS header/name envelope on top of that
// payload without accepting an unbounded chunk.
constexpr uint64_t kMaxAggregateBytes = 84ull * 1024ull * 1024ull;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kRecordHeaderBytes = 28;

uint32_t ReadU32(const uint8_t* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

uint64_t ReadU64(const uint8_t* bytes) {
    uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool SamePath(const char* left, const char* right) {
    if (!left || !right) return false;
    const auto fold = [](char value) {
        if (value == '\\') return '/';
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    };
    while (*left && *right) if (fold(*left++) != fold(*right++)) return false;
    return !*left && !*right;
}

bool ReadPackage(void* context, uint64_t offset, void* destination, size_t size) {
    auto* file = static_cast<std::FILE*>(context);
    return file && offset <= LONG_MAX && size <= LONG_MAX - offset
        && std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0
        && std::fread(destination, 1, size, file) == size;
}

uint64_t PackageSize(std::FILE* file) {
    if (!file || std::fseek(file, 0, SEEK_END) != 0) return 0;
    const long size = std::ftell(file);
    return size > 0 ? static_cast<uint64_t>(size) : 0;
}

bool IsRuntimeSound(const std::string& name) {
    static constexpr std::string_view kRuntimeNames[] = {
        "amb_emitters/emt_chain_ratte_ship1.wav",
        "foley/wpfoly_m4_reload_chamber.wav",
        "foley/wpfoly_m4_reload_clipin.wav",
        "foley/wpfoly_m4_reload_clipout.wav",
        "foley/wpfoly_mp5_reload_chamber_v3.wav",
        "foley/wpfoly_mp5_reload_clipin_v4.wav",
        "foley/wpfoly_mp5_reload_clipout_v4.wav",
        "foley/wpfoly_usp_reload_chamber_v1.wav",
        "foley/wpfoly_usp_reload_clipin_v1.wav",
        "foley/wpfoly_usp_reload_clipout_v2.wav",
        "footsteps/foot_metal04.wav",
        "footsteps/foot_wood02.wav",
        "user_interface/ui_over_v1.wav",
        "user_interface/ui_computer_text_blip1x.wav",
        "user_interface/ui_computer_text_delete1.wav",
        "user_interface/ui_mp_countdown_v1.wav",
        "user_interface/ui_screen_in1.wav",
        "user_interface/ui_screen_out1.wav",
        "user_interface/ui_select_v1.wav",
        "user_interface/ui_sub_over_v1.wav",
        "weapons/colt45/weap_usp45_slst_y1b.wav",
        "weapons/m4/weap_m4_slmn_2c1.wav",
        "weapons/m4/weap_m4_slst_1c4.wav",
        "weapons/mp5/weap_mp5_silencer_slst_2d.wav",
        "weapons/mp5/weap_mp5_slst_4.wav",
    };
    for (const std::string_view candidate : kRuntimeNames) {
        if (name == candidate)
            return true;
    }
    return false;
}

uint32_t Crc32Update(uint32_t crc, const uint8_t* bytes, size_t size) {
    if (!bytes)
        return crc;
    for (size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(crc & 1u)));
    }
    return crc;
}

} // namespace

void VitaSoundBundle::Clear() {
    sounds_.clear();
    aliases_.clear();
    database_ = nullptr;
    ownedDatabase_.Unmount();
#if KISAK_VITA_CPU_RUNTIME
    commonWorker_.Close();primaryWorker_.Close();
#endif
    if (commonFile_) std::fclose(commonFile_);
    if (primaryFile_) std::fclose(primaryFile_);
    commonFile_ = primaryFile_ = nullptr;
    preparationFrozen_ = false;
    loaded_ = false;
}

bool VitaSoundBundle::Load(const VitaAssetDatabase& database) {
    Clear();
    if (LoadRecords(database)) return true;
    Clear();
    return false;
}

bool VitaSoundBundle::LoadPackage(const char* path, const char* commonPath) {
    Clear();
    if (!path || !*path) return false;
#if KISAK_VITA_CPU_RUNTIME
    bool mounted=primaryWorker_.Open(path) && ownedDatabase_.Mount(
        VitaCpuFile::Reader,&primaryWorker_,primaryWorker_.Size())==KvapError::Ok;
    if(mounted && commonPath && *commonPath && std::strcmp(path,commonPath))
        mounted=commonWorker_.Open(commonPath) && ownedDatabase_.MountOverlay(
            VitaCpuFile::Reader,&commonWorker_,commonWorker_.Size())==KvapError::Ok;
#else
    primaryFile_ = std::fopen(path, "rb");
    bool mounted = primaryFile_ && ownedDatabase_.Mount(
        ReadPackage, primaryFile_, PackageSize(primaryFile_)) == KvapError::Ok;
    if (mounted && commonPath && *commonPath && std::strcmp(path, commonPath)) {
        commonFile_ = std::fopen(commonPath, "rb");
        mounted = commonFile_ && ownedDatabase_.MountOverlay(
            ReadPackage, commonFile_, PackageSize(commonFile_)) == KvapError::Ok;
    }
#endif
    if (mounted && LoadRecords(ownedDatabase_)) return true;
    Clear();
    return false;
}

bool VitaSoundBundle::LoadRecords(const VitaAssetDatabase& database) {
    database_ = &database;
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Sound) || chunk->rawSize < 20) continue;
        uint8_t header[20];
        if (database.ReadRange(chunk, 0, header, sizeof(header)) != KvapError::Ok) return false;
        if (ReadU32(header) != kVitaSoundAliasMagic) continue;
        const uint32_t count = ReadU32(header + 8);
        if (ReadU32(header + 4) != kVitaSoundAliasVersion || !count || count > kVitaSoundAliasMaxRecords
            || ReadU64(header + 12) != uint64_t(count) * sizeof(VitaSoundAliasRecord)
            || chunk->rawSize != 20 + uint64_t(count) * sizeof(VitaSoundAliasRecord)) return false;
        uint32_t crc = Crc32Update(0xFFFFFFFFu, header, sizeof(header));
        std::vector<AliasRange> ranges;
        for (uint32_t row = 0; row < count; ++row) {
            VitaSoundAliasRecord record;
            const uint64_t offset = 20 + uint64_t(row) * sizeof(record);
            if (database.ReadRange(chunk, offset, &record, sizeof(record)) != KvapError::Ok) return false;
            crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&record), sizeof(record));
            for (const char* name : {record.aliasName, record.subtitle, record.secondaryAliasName,
                     record.chainAliasName, record.soundFile, record.curveName, record.speakerMapName})
                if (!std::memchr(name, 0, 128)) return false;
            if (!record.aliasName[0] || record.curveKnotCount > 8 || record.soundType > 2
                || record.sourceFileExists > 1 || record.speakerMapDefault > 1) return false;
            const float values[] = {record.volMin, record.volMax, record.pitchMin, record.pitchMax,
                record.distMin, record.distMax, record.slavePercentage, record.probability,
                record.lfePercentage, record.centerPercentage, record.envelopMin, record.envelopMax,
                record.envelopPercentage};
            for (float value : values) if (!std::isfinite(value)) return false;
            for (const auto& knot : record.curveKnots)
                if (!std::isfinite(knot[0]) || !std::isfinite(knot[1])) return false;
            for (const auto& rowMap : record.speakerMaps) for (const auto& map : rowMap) {
                if (map.speakerCount < 0 || map.speakerCount > 6) return false;
                for (const auto& speaker : map.speakers)
                    if (speaker.numLevels < 0 || speaker.numLevels > 2
                        || !std::isfinite(speaker.levels[0]) || !std::isfinite(speaker.levels[1])) return false;
            }
            if (!ranges.empty() && ranges.back().name == record.aliasName) {
                if (++ranges.back().count > 256) return false;
            } else {
                if (!ranges.empty() && ranges.back().name > record.aliasName) return false;
                ranges.push_back({record.aliasName, chunk, offset, 1});
            }
        }
        if (~crc != chunk->crc32) return false;
        for (auto& range : ranges) {
            const auto existing = std::find_if(aliases_.begin(), aliases_.end(), [&](const auto& item) { return item.name == range.name; });
            if (existing == aliases_.end()) aliases_.push_back(std::move(range));
        }
    }
    std::vector<VitaSoundRecord> combined;
    bool foundStream = false;
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const KvapChunk* chunk = database.Chunk(index);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Sound)
            || chunk->rawSize < kHeaderBytes || chunk->rawSize > kMaxAggregateBytes)
            continue;
        // Sound chunks are intentionally allowed to be much larger than the
        // runtime alias set.  Read the fixed stream in bounded ranges instead
        // of duplicating the complete ~84 MiB PCM payload on Vita.
        uint8_t header[kHeaderBytes]{};
        const auto ReadAt = [&](uint64_t offset, void* destination, size_t size) {
            return database.ReadRange(chunk, offset, destination, size) == KvapError::Ok;
        };
        if (!ReadAt(0, header, sizeof(header)))
            continue;
        if (ReadU32(header) != kMagic || ReadU32(header + 4) != kVersion)
            continue;
        foundStream = true;
        const uint32_t recordCount = ReadU32(header + 8);
        const uint64_t declaredBytes = ReadU64(header + 12);
        if (recordCount > kMaxRecords || declaredBytes > kMaxAggregateBytes
            || declaredBytes > UINT64_MAX - kHeaderBytes
            || chunk->rawSize > kMaxAggregateBytes + (16ull * 1024ull * 1024ull))
            return false;
        uint64_t offset = kHeaderBytes;
        uint64_t aggregateBytes = 0;
        std::vector<VitaSoundRecord> parsed;
        parsed.reserve(recordCount);
        bool valid = true;
        uint32_t crc = Crc32Update(0xFFFFFFFFu, header, sizeof(header));
        for (uint32_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
            uint8_t recordHeader[kRecordHeaderBytes]{};
            if (offset > chunk->rawSize
                || kRecordHeaderBytes > chunk->rawSize - offset
                || !ReadAt(offset, recordHeader, sizeof(recordHeader))) {
                valid = false;
                break;
            }
            crc = Crc32Update(crc, recordHeader, sizeof(recordHeader));
            const uint32_t nameBytes = ReadU32(recordHeader);
            const uint32_t sampleBytes = ReadU32(recordHeader + 4);
            VitaSoundRecord sound;
            sound.rate = ReadU32(recordHeader + 8);
            sound.bits = ReadU32(recordHeader + 12);
            sound.channels = ReadU32(recordHeader + 16);
            sound.samples = ReadU32(recordHeader + 20);
            sound.format = ReadU32(recordHeader + 24);
            offset += kRecordHeaderBytes;
            const uint64_t expectedBytes = static_cast<uint64_t>(sound.samples)
                * sound.channels * (sound.bits / 8u);
            if (sampleBytes > kMaxSampleBytes || sound.rate < 8000
                || sound.rate > 96000 || sound.bits != 16
                || (sound.channels != 1 && sound.channels != 2)
                || sound.format != 1 || sound.samples == 0 || nameBytes == 0
                || nameBytes > kMaxNameBytes || expectedBytes != sampleBytes
                || offset > chunk->rawSize
                || nameBytes > chunk->rawSize - offset) {
                valid = false;
                break;
            }
            std::string name;
            name.resize(nameBytes);
            if (!ReadAt(offset, name.data(), nameBytes)) {
                valid = false;
                break;
            }
            crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(name.data()), name.size());
            offset += nameBytes;
            sound.name = std::move(name);
            sound.sourceChunk = chunk;
            sound.sourceOffset = offset;
            sound.sourceBytes = sampleBytes;
            const bool keepSound = IsRuntimeSound(sound.name);
            if (sampleBytes > chunk->rawSize - offset) {
                valid = false;
                break;
            }
            if (keepSound) {
                sound.bytes.resize(sampleBytes);
                if (!ReadAt(offset, sound.bytes.data(), sound.bytes.size())) {
                    valid = false;
                    break;
                }
                crc = Crc32Update(crc, sound.bytes.data(), sound.bytes.size());
            } else {
                // A moderate streaming buffer keeps skipped PCM records off
                // the heap while avoiding one file seek per audio sample.
                std::array<uint8_t, 64u * 1024u> scratch{};
                uint64_t remaining = sampleBytes;
                uint64_t readOffset = offset;
                while (remaining != 0) {
                    const size_t request = static_cast<size_t>(
                        std::min<uint64_t>(remaining, scratch.size()));
                    if (!ReadAt(readOffset, scratch.data(), request)) {
                        valid = false;
                        break;
                    }
                    crc = Crc32Update(crc, scratch.data(), request);
                    readOffset += request;
                    remaining -= request;
                }
                if (!valid)
                    break;
            }
            offset += sampleBytes;
            aggregateBytes += sampleBytes;
            if (aggregateBytes > kMaxAggregateBytes) {
                valid = false;
                break;
            }
            parsed.push_back(std::move(sound));
        }
        if (!valid || offset != chunk->rawSize || aggregateBytes != declaredBytes
            || ~crc != chunk->crc32)
            return false;
        // Primary map chunks are enumerated before the common overlay. Keep
        // the primary record if a compatibility package duplicates a name;
        // newly split packs normally contain disjoint map/common VITS sets.
        for (VitaSoundRecord& sound : parsed) {
            const auto duplicate = std::find_if(
                combined.begin(), combined.end(), [&sound](const VitaSoundRecord& item) {
                    return item.name == sound.name;
                });
            if (duplicate == combined.end())
                combined.push_back(std::move(sound));
        }
    }
    if (foundStream && !combined.empty()) {
        sounds_ = std::move(combined);
        std::sort(sounds_.begin(), sounds_.end(),
                  [](const VitaSoundRecord& left, const VitaSoundRecord& right) {
                      return left.name < right.name;
                  });
        loaded_ = true;
        return true;
    }
    return false;
}

const VitaSoundRecord* VitaSoundBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(sounds_.begin(), sounds_.end(),
                                    [name](const VitaSoundRecord& sound) {
                                        return SamePath(sound.name.c_str(), name);
                                    });
    return found == sounds_.end() ? nullptr : &*found;
}

const VitaSoundRecord* VitaSoundBundle::PreparePcm(const char* name) const {
    if (!name || !database_) return nullptr;
    auto found = std::find_if(sounds_.begin(), sounds_.end(), [name](const auto& item) { return SamePath(item.name.c_str(), name); });
    if (found == sounds_.end()) return nullptr;
    if (!found->bytes.empty()) return &*found;
    constexpr uint64_t kPcmPreparationLimit = 40ull << 20;
    if (preparationFrozen_ || PcmBytes() + found->sourceBytes > kPcmPreparationLimit) return nullptr;
    found->bytes.resize(found->sourceBytes);
    if (database_->ReadRange(found->sourceChunk, found->sourceOffset, found->bytes.data(), found->bytes.size()) != KvapError::Ok) {
        std::vector<uint8_t>().swap(found->bytes);
        return nullptr;
    }
    return &*found;
}

bool VitaSoundBundle::ReadAlias(const char* name, std::vector<VitaSoundAliasRecord>* output) const {
    if (!name || !output || !database_ || preparationFrozen_) return false;
    const auto found = std::find_if(aliases_.begin(), aliases_.end(), [name](const auto& item) { return item.name == name; });
    if (found == aliases_.end()) return false;
    output->resize(found->count);
    return database_->ReadRange(found->chunk, found->offset, output->data(), output->size() * sizeof(output->front())) == KvapError::Ok;
}

uint64_t VitaSoundBundle::PcmBytes() const {
    uint64_t total = 0;
    for (const VitaSoundRecord& sound : sounds_)
        total += sound.bytes.size();
    return total;
}

const VitaSoundRecord* VitaSoundBundle::Record(uint32_t index) const {
    return index < sounds_.size() ? &sounds_[index] : nullptr;
}

} // namespace kisak::vita
