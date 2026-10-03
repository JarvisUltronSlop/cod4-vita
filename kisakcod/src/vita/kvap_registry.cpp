#include <vita/kvap_registry.h>

namespace kisak::vita {

KvapError KvapRegistry::Mount(KvapReadFn read, void* context, uint64_t fileSize) {
    mounted_ = false;
    const KvapError error = reader_.Open(read, context, fileSize, toc_, kMaxChunks);
    mounted_ = error == KvapError::Ok;
    return error;
}

void KvapRegistry::Unmount() {
    mounted_ = false;
}

const KvapChunk* KvapRegistry::Find(KvapChunkType type, uint64_t assetId) const {
    if (!mounted_)
        return nullptr;
    const uint32_t rawType = static_cast<uint32_t>(type);
    for (uint32_t i = 0; i < reader_.ChunkCount(); ++i) {
        const KvapChunk* chunk = reader_.Chunk(i);
        if (chunk && chunk->type == rawType && chunk->assetId == assetId)
            return chunk;
    }
    return nullptr;
}

KvapError KvapRegistry::Read(const KvapChunk* chunk, void* destination, size_t destinationSize) const {
    if (!mounted_ || !chunk)
        return KvapError::InvalidArgument;
    for (uint32_t i = 0; i < reader_.ChunkCount(); ++i) {
        if (reader_.Chunk(i) == chunk)
            return reader_.ReadChunk(i, destination, destinationSize);
    }
    return KvapError::InvalidArgument;
}

KvapError KvapRegistry::ReadRange(const KvapChunk* chunk, uint64_t offset,
                                  void* destination, size_t size) const {
    if (!mounted_ || !chunk)
        return KvapError::InvalidArgument;
    for (uint32_t i = 0; i < reader_.ChunkCount(); ++i) {
        if (reader_.Chunk(i) == chunk)
            return reader_.ReadChunkRange(i, offset, destination, size);
    }
    return KvapError::InvalidArgument;
}

} // namespace kisak::vita
