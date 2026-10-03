#include <vita/vita_animation_bundle.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#include <vita/vita_animation_record.h>

namespace kisak::vita {
namespace {

constexpr uint32_t kMagic = 0x51544956; // "VITQ"
constexpr uint32_t kVersionV1 = 1;
constexpr uint32_t kVersionV2 = 2;
constexpr uint32_t kVersionV3 = kVitaAnimationVersionSource;
constexpr uint32_t kRecordBytesV1 = 182;
constexpr uint32_t kRecordBytesV2 = 162;
constexpr uint32_t kRecordBytesV3 = sizeof(VitaAnimationRecordV3);
constexpr uint32_t kMaxClips = 2048;
constexpr uint32_t kMaxTracksPerClip = 64;
constexpr uint32_t kMaxBonesPerTrack = 128;
constexpr uint32_t kMaxFramesPerTrack = 256;
constexpr uint32_t kMaxNotifiesPerClip = 128;
constexpr uint64_t kMaxChunkBytes = 16ull * 1024ull * 1024ull;

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset) {
    uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

float ReadFloat(const std::vector<uint8_t>& bytes, size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool ReadName(const std::vector<uint8_t>& bytes, size_t offset, std::string* output) {
    if (!output || offset + 128 > bytes.size())
        return false;
    const char* name = reinterpret_cast<const char*>(bytes.data() + offset);
    size_t length = 0;
    while (length < 128 && name[length] != '\0')
        ++length;
    if (length == 0 || length == 128)
        return false;
    output->assign(name, length);
    return true;
}

bool ReadFixedString(const std::vector<uint8_t>& bytes, size_t offset,
                     size_t width, std::string* output) {
    if (!output || width == 0 || offset > bytes.size()
        || width > bytes.size() - offset)
        return false;
    const char* value = reinterpret_cast<const char*>(bytes.data() + offset);
    size_t length = 0;
    while (length < width && value[length] != '\0')
        ++length;
    if (length == 0 || length == width)
        return false;
    for (size_t index = length + 1; index < width; ++index) {
        if (value[index] != '\0')
            return false;
    }
    output->assign(value, length);
    return true;
}

template <typename T>
bool ReadArray(const std::vector<uint8_t>& bytes, size_t end,
               size_t* offset, uint32_t count, std::vector<T>* output) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!offset || !output)
        return false;
    const uint64_t byteCount = static_cast<uint64_t>(count) * sizeof(T);
    if (*offset > end || end > bytes.size() || byteCount > end - *offset)
        return false;
    output->resize(count);
    if (byteCount != 0)
        std::memcpy(output->data(), bytes.data() + *offset,
                    static_cast<size_t>(byteCount));
    *offset += static_cast<size_t>(byteCount);
    return true;
}

bool ParseSampledTracks(const std::vector<uint8_t>& bytes, size_t end,
                        size_t* offset, uint16_t numFrames,
                        uint32_t trackCount,
                        std::vector<VitaAnimationTrack>* output) {
    if (!offset || !output || trackCount > kMaxTracksPerClip)
        return false;
    output->reserve(trackCount);
    for (uint32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        if (*offset > end || end - *offset < 16)
            return false;
        const uint32_t modelNameBytes = ReadU32(bytes, *offset);
        const uint32_t boneCount = ReadU32(bytes, *offset + 4);
        const uint32_t frameCount = ReadU32(bytes, *offset + 8);
        const uint32_t reserved = ReadU32(bytes, *offset + 12);
        if (modelNameBytes == 0 || modelNameBytes > 256
            || boneCount == 0 || boneCount > kMaxBonesPerTrack
            || frameCount == 0 || frameCount > kMaxFramesPerTrack
            || numFrames == 0 || frameCount != numFrames || reserved != 0)
            return false;
        *offset += 16;
        if (*offset > end || modelNameBytes > end - *offset
            || std::memchr(bytes.data() + *offset, '\0', modelNameBytes))
            return false;
        VitaAnimationTrack track{};
        track.modelName.assign(
            reinterpret_cast<const char*>(bytes.data() + *offset),
            modelNameBytes);
        *offset += modelNameBytes;
        const uint64_t sampleCount = static_cast<uint64_t>(boneCount)
            * frameCount;
        if (sampleCount > std::numeric_limits<size_t>::max()
            / sizeof(VitaAnimationSample))
            return false;
        const uint64_t sampleBytes = sampleCount * sizeof(VitaAnimationSample);
        if (*offset > end || sampleBytes > end - *offset)
            return false;
        track.boneCount = boneCount;
        track.frameCount = frameCount;
        track.samples.resize(static_cast<size_t>(sampleCount));
        std::memcpy(track.samples.data(), bytes.data() + *offset,
                    static_cast<size_t>(sampleBytes));
        for (const VitaAnimationSample& sample : track.samples) {
            for (float value : sample.quat)
                if (!std::isfinite(value)) return false;
            for (float value : sample.trans)
                if (!std::isfinite(value)) return false;
            if (!std::isfinite(sample.transWeight)
                || sample.transWeight < 0.0f)
                return false;
        }
        output->push_back(std::move(track));
        *offset += static_cast<size_t>(sampleBytes);
    }
    return true;
}

// DB_FindXAssetHeader in KisakCOD compares asset names with I_stricmp. Keep
// that source lookup contract when a Windows-exported WeaponDef preserves a
// different case than the canonical xanim asset name in the package.
bool SourceNameEquals(const std::string& left, const char* right) {
    if (!right)
        return false;
    size_t index = 0;
    for (; index < left.size() && right[index] != '\0'; ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return index == left.size() && right[index] == '\0';
}

} // namespace

const VitaAnimationSample* VitaAnimationTrack::Sample(uint32_t frame,
                                                       uint32_t bone) const {
    if (frame >= frameCount || bone >= boneCount)
        return nullptr;
    const size_t index = static_cast<size_t>(frame) * boneCount + bone;
    return index < samples.size() ? &samples[index] : nullptr;
}

const VitaAnimationTrack* VitaAnimationClip::TrackForModel(const char* modelName) const {
    if (!modelName || !*modelName)
        return nullptr;
    const auto found = std::find_if(tracks.begin(), tracks.end(),
                                    [modelName](const VitaAnimationTrack& track) {
                                        return track.modelName == modelName;
                                    });
    return found == tracks.end() ? nullptr : &*found;
}

float VitaAnimationClip::DurationSeconds() const {
    if (framerate <= 0.0f || numFrames <= 1)
        return 0.0f;
    return static_cast<float>(numFrames - 1) / framerate;
}

bool VitaAnimationClip::SourceComplete() const {
    const uint32_t indexStride = numFrames >= 256 ? 2u : 1u;
    uint32_t quatParts = 0;
    uint32_t transParts = 0;
    for (uint32_t index = 0; index < 5; ++index)
        quatParts += boneCount[index];
    for (uint32_t index = 5; index < 9; ++index)
        transParts += boneCount[index];
    if (!sourceCompressed || boneNames.size() != boneCount[9]
        || quatParts != boneCount[9] || transParts != boneCount[9]
        || dataByte.size() != dataByteCount
        || dataShort.size() != dataShortCount
        || dataInt.size() != dataIntCount
        || randomDataByte.size() != randomDataByteCount
        || randomDataShort.size() != randomDataShortCount
        || randomDataInt.size() != randomDataIntCount
        || indices.size() != static_cast<uint64_t>(indexCount) * indexStride
        || notifies.size() != notifyCount
        || deltaPart.present != delta)
        return false;
    if (!delta)
        return true;
    const uint64_t transSamples = static_cast<uint64_t>(deltaPart.transSize) + 1u;
    const uint64_t quatSamples = static_cast<uint64_t>(deltaPart.quatSize) + 1u;
    if (deltaPart.hasTrans) {
        if (deltaPart.transSize > numFrames)
            return false;
        const uint64_t frameBytes = deltaPart.transSize == 0 ? 0u
            : transSamples * (deltaPart.transSmall ? 3u : 6u);
        const uint64_t indexBytes = deltaPart.transSize == 0 ? 0u
            : transSamples * indexStride;
        if (deltaPart.transFrames.size() != frameBytes
            || deltaPart.transIndices.size() != indexBytes)
            return false;
    } else if (deltaPart.transSize != 0 || deltaPart.transSmall
               || !deltaPart.transFrames.empty()
               || !deltaPart.transIndices.empty()) {
        return false;
    }
    if (deltaPart.hasQuat) {
        if (deltaPart.quatSize > numFrames)
            return false;
        const uint64_t frameBytes = deltaPart.quatSize == 0 ? 0u
            : quatSamples * 2u * sizeof(int16_t);
        const uint64_t indexBytes = deltaPart.quatSize == 0 ? 0u
            : quatSamples * indexStride;
        if (deltaPart.quatFrames.size() != frameBytes
            || deltaPart.quatIndices.size() != indexBytes)
            return false;
    } else if (deltaPart.quatSize != 0 || !deltaPart.quatFrames.empty()
               || !deltaPart.quatIndices.empty()) {
        return false;
    }
    return true;
}

void VitaAnimationBundle::Clear() {
    std::vector<VitaAnimationClip>().swap(clips_);
    loaded_ = false;
    wireVersion_ = 0;
    stagingCapacityBytes_ = 0;
    loadError_ = "";
}

bool VitaAnimationBundle::SourceComplete() const {
    return loaded_ && wireVersion_ == kVersionV3
        && !clips_.empty()
        && std::all_of(clips_.begin(), clips_.end(),
                       [](const VitaAnimationClip& clip) {
                           return clip.SourceComplete();
                       });
}

uint32_t VitaAnimationBundle::NotifyCount() const {
    uint32_t count = 0;
    for (const VitaAnimationClip& clip : clips_)
        count += static_cast<uint32_t>(clip.notifies.size());
    return count;
}

bool VitaAnimationBundle::Load(const VitaAssetDatabase& database) try {
    Clear();
    loadError_ = "animation chunk missing, unreadable, or invalid";
    for (uint32_t chunkIndex = 0; chunkIndex < database.ChunkCount(); ++chunkIndex) {
        const KvapChunk* chunk = database.Chunk(chunkIndex);
        if (!chunk || chunk->type != static_cast<uint32_t>(VitaAssetType::Animation)
            || chunk->rawSize < 16 || chunk->rawSize > kMaxChunkBytes)
            continue;
        // Keep only one wire record alongside the decoded clips. A whole-chunk
        // staging copy can require a 14 MiB contiguous allocation on map reload.
        std::vector<uint8_t> bytes(16);
        if (database.ReadRange(chunk, 0, bytes.data(), bytes.size()) != KvapError::Ok)
            continue;
        uint32_t crc = KvapCrc32Update(0xFFFFFFFFu, bytes.data(), bytes.size());
        const uint32_t magic = ReadU32(bytes, 0);
        const uint32_t version = ReadU32(bytes, 4);
        const uint32_t count = ReadU32(bytes, 8);
        const uint32_t recordBytes = ReadU32(bytes, 12);
        if (magic != kMagic
            || (version != kVersionV1 && version != kVersionV2
                && version != kVersionV3)
            || count == 0 || count > kMaxClips
            || (version == kVersionV1 && recordBytes != kRecordBytesV1)
            || (version == kVersionV2 && recordBytes != kRecordBytesV2)
            || (version == kVersionV3 && recordBytes != kRecordBytesV3))
            continue;

        std::vector<VitaAnimationClip> parsed;
        parsed.reserve(count);
        bool valid = true;
        uint64_t chunkOffset = 16;
        for (uint32_t index = 0; index < count; ++index) {
            if (chunkOffset > chunk->rawSize
                || recordBytes > chunk->rawSize - chunkOffset) {
                valid = false;
                break;
            }
            bytes.resize(recordBytes);
            if (database.ReadRange(chunk, chunkOffset, bytes.data(), recordBytes)
                != KvapError::Ok) {
                valid = false;
                break;
            }
            uint64_t payloadBytes = 0;
            if (version == kVersionV2) {
                payloadBytes = ReadU32(bytes, 150);
            } else if (version == kVersionV3) {
                VitaAnimationRecordV3 record{};
                std::memcpy(&record, bytes.data(), sizeof(record));
                payloadBytes = static_cast<uint64_t>(record.nativePayloadBytes)
                    + record.sampledPayloadBytes;
            }
            if (payloadBytes > chunk->rawSize - chunkOffset - recordBytes) {
                valid = false;
                break;
            }
            const size_t stagedBytes = recordBytes + static_cast<size_t>(payloadBytes);
            bytes.reserve(stagedBytes);
            bytes.resize(stagedBytes);
            stagingCapacityBytes_ = std::max(stagingCapacityBytes_, bytes.capacity());
            if (payloadBytes != 0
                && database.ReadRange(chunk, chunkOffset + recordBytes,
                                      bytes.data() + recordBytes,
                                      static_cast<size_t>(payloadBytes)) != KvapError::Ok) {
                valid = false;
                break;
            }
            crc = KvapCrc32Update(crc, bytes.data(), bytes.size());
            size_t offset = 0;
            VitaAnimationClip clip{};
            if (!ReadName(bytes, offset, &clip.name)) {
                valid = false;
                break;
            }
            clip.numFrames = ReadU16(bytes, offset + 128);
            clip.loop = bytes[offset + 130] != 0;
            clip.delta = bytes[offset + 131] != 0;
            std::memcpy(clip.boneCount, bytes.data() + offset + 132,
                        sizeof(clip.boneCount));
            clip.notifyCount = bytes[offset + 142];
            clip.assetType = bytes[offset + 143];
            if (version == kVersionV1) {
                clip.dataByteCount = ReadU32(bytes, offset + 146);
                clip.dataShortCount = ReadU32(bytes, offset + 150);
                clip.dataIntCount = ReadU32(bytes, offset + 154);
                clip.randomDataByteCount = ReadU32(bytes, offset + 158);
                clip.randomDataIntCount = ReadU32(bytes, offset + 162);
                clip.randomDataShortCount = ReadU32(bytes, offset + 166);
                clip.indexCount = ReadU32(bytes, offset + 170);
                clip.framerate = ReadFloat(bytes, offset + 174);
                clip.frequency = ReadFloat(bytes, offset + 178);
            } else if (version == kVersionV2) {
                // VITQ v2 replaces the seven desktop array counts with a
                // track count/payload size and moves timing to offsets 154/158.
                clip.framerate = ReadFloat(bytes, offset + 154);
                clip.frequency = ReadFloat(bytes, offset + 158);
            } else {
                VitaAnimationRecordV3 record{};
                std::memcpy(&record, bytes.data() + offset, sizeof(record));
                uint32_t quatParts = 0;
                uint32_t transParts = 0;
                for (uint32_t boneIndex = 0; boneIndex < 5; ++boneIndex)
                    quatParts += record.boneCount[boneIndex];
                for (uint32_t boneIndex = 5; boneIndex < 9; ++boneIndex)
                    transParts += record.boneCount[boneIndex];
                if (record.bLoop > 1 || record.bDelta > 1
                    || record.isDefault > 1 || record.reserved8 != 0
                    || record.reserved16 != 0
                    || record.boneNameCount != record.boneCount[9]
                    || quatParts != record.boneCount[9]
                    || transParts != record.boneCount[9]
                    || record.boneNameCount > kMaxBonesPerTrack
                    || record.notifyCount > kMaxNotifiesPerClip
                    || record.trackCount > kMaxTracksPerClip
                    || record.nativePayloadBytes > kMaxChunkBytes
                    || record.sampledPayloadBytes > kMaxChunkBytes
                    || (record.bDelta == 0 && record.deltaPayloadBytes != 0)
                    || (record.bDelta != 0
                        && record.deltaPayloadBytes < sizeof(VitaAnimationDeltaV3))) {
                    valid = false;
                    break;
                }
                clip.isDefault = record.isDefault != 0;
                clip.dataByteCount = record.dataByteCount;
                clip.dataShortCount = record.dataShortCount;
                clip.dataIntCount = record.dataIntCount;
                clip.randomDataByteCount = record.randomDataByteCount;
                clip.randomDataIntCount = record.randomDataIntCount;
                clip.randomDataShortCount = record.randomDataShortCount;
                clip.indexCount = record.indexCount;
                clip.framerate = record.framerate;
                clip.frequency = record.frequency;
            }
            if (!std::isfinite(clip.framerate) || !std::isfinite(clip.frequency)
                || clip.framerate < 0.0f || clip.frequency < 0.0f) {
                valid = false;
                break;
            }
            if (version == kVersionV2) {
                const uint32_t trackCount = ReadU32(bytes, offset + 146);
                const uint32_t payloadBytes = ReadU32(bytes, offset + 150);
                if (trackCount > kMaxTracksPerClip
                    || payloadBytes > kMaxChunkBytes
                    || static_cast<uint64_t>(offset) + recordBytes + payloadBytes > bytes.size()) {
                    valid = false;
                    break;
                }
                size_t trackOffset = offset + recordBytes;
                const size_t payloadEnd = trackOffset + payloadBytes;
                valid = ParseSampledTracks(bytes, payloadEnd, &trackOffset,
                                           clip.numFrames, trackCount,
                                           &clip.tracks);
                // VITQ v2 originally ended after the sampled tracks. Newer
                // exporters append a bounded variable-length notetrack tail
                // while retaining the same record version, so old packages
                // remain valid when no tail is present.
                if (valid && trackOffset < payloadEnd) {
                    if (clip.notifyCount == 0
                        || clip.notifyCount > kMaxNotifiesPerClip)
                        valid = false;
                    clip.notifies.reserve(clip.notifyCount);
                    for (uint32_t notifyIndex = 0;
                         valid && notifyIndex < clip.notifyCount; ++notifyIndex) {
                        if (trackOffset > payloadEnd
                            || payloadEnd - trackOffset < 8) {
                            valid = false;
                            break;
                        }
                        const uint32_t nameBytes = ReadU32(bytes, trackOffset);
                        const float notifyTime = ReadFloat(bytes, trackOffset + 4);
                        trackOffset += 8;
                        if (nameBytes == 0 || nameBytes > 256
                            || nameBytes > payloadEnd - trackOffset
                            || !std::isfinite(notifyTime)
                            || notifyTime < 0.0f || notifyTime > 1.0f) {
                            valid = false;
                            break;
                        }
                        VitaAnimationNotify notify{};
                        notify.name.assign(
                            reinterpret_cast<const char*>(bytes.data() + trackOffset),
                            nameBytes);
                        notify.time = notifyTime;
                        clip.notifies.push_back(std::move(notify));
                        trackOffset += nameBytes;
                    }
                }
                if (!valid || trackOffset != payloadEnd) {
                    valid = false;
                    break;
                }
                offset = payloadEnd;
            } else if (version == kVersionV3) {
                VitaAnimationRecordV3 record{};
                std::memcpy(&record, bytes.data() + offset, sizeof(record));
                const uint32_t indexStride = clip.numFrames >= 256 ? 2u : 1u;
                const uint64_t indexBytes = static_cast<uint64_t>(
                    record.indexCount) * indexStride;
                uint64_t expectedNative = static_cast<uint64_t>(
                    record.boneNameCount) * sizeof(VitaAnimationBoneNameV3);
                expectedNative += record.dataByteCount;
                expectedNative += static_cast<uint64_t>(record.dataShortCount)
                    * sizeof(int16_t);
                expectedNative += static_cast<uint64_t>(record.dataIntCount)
                    * sizeof(int32_t);
                expectedNative += record.randomDataByteCount;
                expectedNative += static_cast<uint64_t>(record.randomDataShortCount)
                    * sizeof(int16_t);
                expectedNative += static_cast<uint64_t>(record.randomDataIntCount)
                    * sizeof(int32_t);
                expectedNative += indexBytes;
                expectedNative += static_cast<uint64_t>(record.notifyCount)
                    * sizeof(VitaAnimationNotifyV3);
                expectedNative += record.deltaPayloadBytes;
                const uint64_t recordEnd = static_cast<uint64_t>(offset)
                    + recordBytes + record.nativePayloadBytes
                    + record.sampledPayloadBytes;
                if (expectedNative != record.nativePayloadBytes
                    || indexBytes > UINT32_MAX || recordEnd > bytes.size()) {
                    valid = false;
                    break;
                }

                size_t nativeOffset = offset + recordBytes;
                const size_t nativeEnd = nativeOffset + record.nativePayloadBytes;
                clip.boneNames.reserve(record.boneNameCount);
                for (uint32_t boneIndex = 0;
                     valid && boneIndex < record.boneNameCount; ++boneIndex) {
                    std::string boneName;
                    if (!ReadFixedString(bytes, nativeOffset,
                                         sizeof(VitaAnimationBoneNameV3),
                                         &boneName)) {
                        valid = false;
                        break;
                    }
                    clip.boneNames.push_back(std::move(boneName));
                    nativeOffset += sizeof(VitaAnimationBoneNameV3);
                }
                if (valid)
                    valid = ReadArray(bytes, nativeEnd, &nativeOffset,
                                      record.dataByteCount, &clip.dataByte)
                        && ReadArray(bytes, nativeEnd, &nativeOffset,
                                     record.dataShortCount, &clip.dataShort)
                        && ReadArray(bytes, nativeEnd, &nativeOffset,
                                     record.dataIntCount, &clip.dataInt)
                        && ReadArray(bytes, nativeEnd, &nativeOffset,
                                     record.randomDataByteCount,
                                     &clip.randomDataByte)
                        && ReadArray(bytes, nativeEnd, &nativeOffset,
                                     record.randomDataShortCount,
                                     &clip.randomDataShort)
                        && ReadArray(bytes, nativeEnd, &nativeOffset,
                                     record.randomDataIntCount,
                                     &clip.randomDataInt)
                        && ReadArray(bytes, nativeEnd, &nativeOffset,
                                     static_cast<uint32_t>(indexBytes),
                                     &clip.indices);

                clip.notifies.reserve(record.notifyCount);
                float previousNotifyTime = -1.0f;
                for (uint32_t notifyIndex = 0;
                     valid && notifyIndex < record.notifyCount; ++notifyIndex) {
                    if (nativeOffset > nativeEnd
                        || sizeof(VitaAnimationNotifyV3)
                            > nativeEnd - nativeOffset) {
                        valid = false;
                        break;
                    }
                    std::string notifyName;
                    const float notifyTime = ReadFloat(
                        bytes, nativeOffset + kVitaAnimationNotifyNameBytes);
                    if (!ReadFixedString(bytes, nativeOffset,
                                         kVitaAnimationNotifyNameBytes,
                                         &notifyName)
                        || !std::isfinite(notifyTime)
                        || notifyTime < 0.0f || notifyTime > 1.0f
                        || notifyTime < previousNotifyTime) {
                        valid = false;
                        break;
                    }
                    clip.notifies.push_back({std::move(notifyName), notifyTime});
                    previousNotifyTime = notifyTime;
                    nativeOffset += sizeof(VitaAnimationNotifyV3);
                }

                if (valid && clip.delta) {
                    if (nativeOffset > nativeEnd
                        || sizeof(VitaAnimationDeltaV3)
                            > nativeEnd - nativeOffset) {
                        valid = false;
                    } else {
                        VitaAnimationDeltaV3 delta{};
                        std::memcpy(&delta, bytes.data() + nativeOffset,
                                    sizeof(delta));
                        const uint32_t deltaIndexStride = clip.numFrames >= 256
                            ? 2u : 1u;
                        const uint64_t transSamples =
                            static_cast<uint64_t>(delta.transSize) + 1u;
                        const uint64_t quatSamples =
                            static_cast<uint64_t>(delta.quatSize) + 1u;
                        const uint64_t expectedTransFrames =
                            delta.hasTrans && delta.transSize != 0
                            ? transSamples * (delta.transSmall ? 3u : 6u) : 0u;
                        const uint64_t expectedTransIndices =
                            delta.hasTrans && delta.transSize != 0
                            ? transSamples * deltaIndexStride : 0u;
                        const uint64_t expectedQuatFrames =
                            delta.hasQuat && delta.quatSize != 0
                            ? quatSamples * 2u * sizeof(int16_t) : 0u;
                        const uint64_t expectedQuatIndices =
                            delta.hasQuat && delta.quatSize != 0
                            ? quatSamples * deltaIndexStride : 0u;
                        const uint64_t expectedDeltaBytes = sizeof(delta)
                            + expectedTransFrames + expectedTransIndices
                            + expectedQuatFrames + expectedQuatIndices;
                        if (delta.hasTrans > 1 || delta.hasQuat > 1
                            || delta.transSmall > 1 || delta.reserved8 != 0
                            || delta.reserved32 != 0
                            || (!delta.hasTrans && (delta.transSize != 0
                                || delta.transSmall != 0))
                            || (!delta.hasQuat && delta.quatSize != 0)
                            || delta.transSize > clip.numFrames
                            || delta.quatSize > clip.numFrames
                            || delta.transFrameBytes != expectedTransFrames
                            || delta.transIndexBytes != expectedTransIndices
                            || delta.quatFrameBytes != expectedQuatFrames
                            || delta.quatIndexBytes != expectedQuatIndices
                            || expectedDeltaBytes != record.deltaPayloadBytes) {
                            valid = false;
                        } else {
                            clip.deltaPart.present = true;
                            clip.deltaPart.hasTrans = delta.hasTrans != 0;
                            clip.deltaPart.hasQuat = delta.hasQuat != 0;
                            clip.deltaPart.transSmall = delta.transSmall != 0;
                            clip.deltaPart.transSize = delta.transSize;
                            clip.deltaPart.quatSize = delta.quatSize;
                            std::memcpy(clip.deltaPart.transBase,
                                        delta.transBase,
                                        sizeof(delta.transBase));
                            std::memcpy(clip.deltaPart.transRange,
                                        delta.transRange,
                                        sizeof(delta.transRange));
                            std::memcpy(clip.deltaPart.quatFrame0,
                                        delta.quatFrame0,
                                        sizeof(delta.quatFrame0));
                            for (float value : delta.transBase)
                                if (!std::isfinite(value)) valid = false;
                            for (float value : delta.transRange)
                                if (!std::isfinite(value)) valid = false;
                            nativeOffset += sizeof(delta);
                            if (valid)
                                valid = ReadArray(bytes, nativeEnd, &nativeOffset,
                                                  delta.transFrameBytes,
                                                  &clip.deltaPart.transFrames)
                                    && ReadArray(bytes, nativeEnd, &nativeOffset,
                                                 delta.transIndexBytes,
                                                 &clip.deltaPart.transIndices)
                                    && ReadArray(bytes, nativeEnd, &nativeOffset,
                                                 delta.quatFrameBytes,
                                                 &clip.deltaPart.quatFrames)
                                    && ReadArray(bytes, nativeEnd, &nativeOffset,
                                                 delta.quatIndexBytes,
                                                 &clip.deltaPart.quatIndices);
                        }
                    }
                }
                if (!valid || nativeOffset != nativeEnd) {
                    valid = false;
                    break;
                }
                clip.sourceCompressed = true;
                size_t sampledOffset = nativeEnd;
                const size_t sampledEnd = sampledOffset
                    + record.sampledPayloadBytes;
                if (!ParseSampledTracks(bytes, sampledEnd, &sampledOffset,
                                        clip.numFrames, record.trackCount,
                                        &clip.tracks)
                    || sampledOffset != sampledEnd) {
                    valid = false;
                    break;
                }
                offset = sampledEnd;
            } else {
                offset += recordBytes;
            }
            if (offset != bytes.size()) {
                valid = false;
                break;
            }
            chunkOffset += bytes.size();
            parsed.push_back(std::move(clip));
        }
        if (valid && parsed.size() == count && chunkOffset == chunk->rawSize
            && ~crc == chunk->crc32) {
            std::sort(parsed.begin(), parsed.end(), [](const VitaAnimationClip& left,
                                                       const VitaAnimationClip& right) {
                return left.name < right.name;
            });
            for (VitaAnimationClip& clip : parsed) {
                std::sort(clip.tracks.begin(), clip.tracks.end(),
                          [](const VitaAnimationTrack& left,
                             const VitaAnimationTrack& right) {
                              return left.modelName < right.modelName;
                          });
            }
            clips_ = std::move(parsed);
            loaded_ = true;
            wireVersion_ = version;
            loadError_ = "";
            return true;
        }
    }
    return false;
} catch (const std::bad_alloc&) {
    // This translation unit enables exceptions locally; no exception crosses
    // the source runtime's exception-free boundary or publishes partial clips.
    const size_t stagingCapacity = stagingCapacityBytes_;
    Clear();
    stagingCapacityBytes_ = stagingCapacity;
    loadError_ = "animation allocation failed";
    return false;
}

const VitaAnimationClip* VitaAnimationBundle::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(clips_.begin(), clips_.end(),
                                    [name](const VitaAnimationClip& clip) {
                                        return SourceNameEquals(clip.name, name);
                                    });
    return found == clips_.end() ? nullptr : &*found;
}

const VitaAnimationTrack* VitaAnimationBundle::FindTrack(const char* animationName,
                                                         const char* modelName) const {
    const VitaAnimationClip* clip = Find(animationName);
    return clip ? clip->TrackForModel(modelName) : nullptr;
}

uint32_t VitaAnimationBundle::CollectNotifies(
    const VitaAnimationClip* clip, float oldPhase, float newPhase, bool wrapped,
    const VitaAnimationNotify** output, uint32_t outputCapacity) const {
    if (!clip || !output || outputCapacity == 0 || clip->notifies.empty())
        return 0;
    oldPhase = std::clamp(oldPhase, 0.0f, 1.0f);
    newPhase = std::clamp(newPhase, 0.0f, 1.0f);
    wrapped = wrapped || newPhase < oldPhase;
    uint32_t count = 0;
    for (const VitaAnimationNotify& notify : clip->notifies) {
        const bool crossed = wrapped
            ? (notify.time > oldPhase || notify.time <= newPhase)
            : (notify.time > oldPhase && notify.time <= newPhase);
        if (!crossed)
            continue;
        output[count++] = &notify;
        if (count == outputCapacity)
            break;
    }
    return count;
}

} // namespace kisak::vita
