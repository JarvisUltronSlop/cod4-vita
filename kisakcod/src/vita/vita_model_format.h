#pragma once
#include <cstddef>
#include <cstdint>

namespace kisak::vita {
// Archive limits cover both maps' source script/model closure. They are
// validation limits, not residency allowances or upfront allocations.
inline constexpr uint32_t kVitaModelMaxRecords = 512;
inline constexpr uint64_t kVitaModelMaxPayloadBytes = 32ull * 1024ull * 1024ull;
} // namespace kisak::vita
