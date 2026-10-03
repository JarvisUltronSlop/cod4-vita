#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

enum class GfxVitaPageUpload { Complete, OutOfMemory, Failed };

// The logical cache limit does not reserve contiguous space in VitaGL's shared
// allocator. On OOM, retire additional unpinned pages before a bounded retry.
// Upload must dispose of partial storage before returning; eviction must fence
// GPU readers and immediately free storage. Neither may change image content.
template <typename Upload, typename Evict, typename Resident>
bool GfxVitaUploadWithPagePressure(uint64_t incoming, uint64_t& budget,
                                  Upload upload, Evict evict, Resident resident) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto result = upload();
        if (result == GfxVitaPageUpload::Complete) return true;
        if (result != GfxVitaPageUpload::OutOfMemory || attempt == 2) return false;
        const uint64_t goal = attempt == 0 ? resident() / 2 : 0;
        bool reclaimed = false;
        for (size_t count = 0; count < 2048 && resident() > goal; ++count) {
            const auto before = resident();
            if (!evict()) break; // Remaining pages can all be pinned.
            if (resident() >= before) return false; // No accounting progress.
            reclaimed = true;
        }
        if (!reclaimed) return false;
        // Remember pressure for this map rather than repeatedly refilling to
        // a known failing footprint. Reset at the next backing-store lifetime.
        budget = std::min(budget, resident() + incoming);
    }
    return false;
}
