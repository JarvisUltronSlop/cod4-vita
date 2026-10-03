#pragma once

#include <cstdint>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

inline constexpr uint32_t kVitaContentSchemaVersion = 2;
inline constexpr uint64_t kVitaNonGpuBudgetBytes = 114ull * 1024ull * 1024ull;
inline constexpr uint64_t kVitaGpuReserveBytes = 64ull * 1024ull * 1024ull;
inline constexpr uint64_t kVitaWorkingBudgetBytes = 178ull * 1024ull * 1024ull;

enum class VitaContentRole : uint8_t {
    Unknown = 0,
    Common,
    Map,
};

enum class VitaContentError : uint8_t {
    Ok = 0,
    InvalidArgument,
    OpenFailed,
    PackageRejected,
    ManifestMissing,
    ManifestTooLarge,
    ManifestCorrupt,
    SchemaMismatch,
    RoleMismatch,
    NotCertified,
    NotRuntimeReady,
    MapRejected,
    BudgetExceeded,
    DependencyMismatch,
};

// Bounded, pointer-free package contract.  The JSON payload is a convenient
// host artifact; only these fixed fields cross into the Vita runtime.
struct VitaContentManifest {
    uint32_t schemaVersion = 0;
    VitaContentRole role = VitaContentRole::Unknown;
    uint32_t mapIndex = UINT32_MAX;
    bool certified = false;
    uint64_t packageId = 0;
    uint64_t commonPackageId = 0;
    uint64_t predictedCpuResidentBytes = 0;
    uint64_t predictedGpuResidentBytes = 0;
    uint8_t sourceInstallHash[32]{};
    char mapId[24]{};
};

VitaContentError VitaContentManifest_Load(const VitaAssetDatabase& database,
                                          VitaContentRole expectedRole,
                                          VitaContentManifest* output);

// Lightweight startup probe.  It verifies the KVAP header/TOC and the
// manifest chunk CRC without loading any map payload.
VitaContentError VitaContentManifest_ProbeFile(const char* path,
                                               VitaContentRole expectedRole,
                                               VitaContentManifest* output);

// A map can enter the selectable catalog only when it names this exact common
// pack, was exported from the same owned PC install, and the pair fits the
// explicit Vita residency envelope.
VitaContentError VitaContentManifest_ValidateDependency(
    const VitaContentManifest& common,
    const VitaContentManifest& map);

const char* VitaContentErrorName(VitaContentError error);

} // namespace kisak::vita
