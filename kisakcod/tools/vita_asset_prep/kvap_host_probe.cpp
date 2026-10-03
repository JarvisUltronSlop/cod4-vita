#include <vita/vita_asset_db.h>

#include <cstdio>
#include <fstream>
#include <vector>

namespace {

struct FileContext {
    std::ifstream file;
};

bool ReadFile(void* rawContext, uint64_t offset, void* destination, size_t size) {
    auto* context = static_cast<FileContext*>(rawContext);
    context->file.seekg(static_cast<std::streamoff>(offset));
    context->file.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return context->file.good() || context->file.gcount() == static_cast<std::streamsize>(size);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: kvap_host_probe <package.kvap>\n");
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input) {
        std::perror(argv[1]);
        return 3;
    }
    const auto fileSize = static_cast<uint64_t>(input.tellg());
    FileContext context{std::move(input)};
    kisak::vita::VitaAssetDatabase database;
    const auto openError = database.Mount(ReadFile, &context, fileSize);
    if (openError != kisak::vita::KvapError::Ok || database.ChunkCount() == 0) {
        std::fprintf(stderr, "open failed=%u chunks=%u\n",
                     static_cast<unsigned>(openError), database.ChunkCount());
        return 4;
    }

    const auto* chunk = database.Chunk(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(chunk->rawSize));
    const auto readError = database.Read(chunk, bytes.data(), bytes.size());
    auto allReadError = kisak::vita::KvapError::Ok;
    for (uint32_t index = 0; index < database.ChunkCount(); ++index) {
        const auto* entry = database.Chunk(index);
        if (!entry)
            return 5;
        std::vector<uint8_t> payload(static_cast<size_t>(entry->rawSize));
        allReadError = database.Read(entry, payload.data(), payload.size());
        if (allReadError != kisak::vita::KvapError::Ok)
            break;
    }
    std::printf("kvap host typed-db probe: chunks=%u manifest=%u strings=%u world=%u referenceOnly=%u playable=%u firstBytes=%zu read=%u allRead=%u crc=%08x\n",
                database.ChunkCount(),
                database.Count(kisak::vita::VitaAssetType::Manifest),
                database.Count(kisak::vita::VitaAssetType::Strings),
                database.Count(kisak::vita::VitaAssetType::World),
                static_cast<unsigned>(database.IsReferenceOnly()),
                static_cast<unsigned>(database.IsPlayableShipment()),
                bytes.size(), static_cast<unsigned>(readError),
                static_cast<unsigned>(allReadError),
                chunk->crc32);
    return readError == kisak::vita::KvapError::Ok
        && allReadError == kisak::vita::KvapError::Ok ? 0 : 6;
}
