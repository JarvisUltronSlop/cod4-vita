#pragma once
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_catalog_key.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_asset_db.h>

namespace kisak::vita {

struct VitaStringTableRecord {
    std::string name;
    uint32_t rowCount = 0;
    uint32_t columnCount = 0;
    std::vector<std::string> values;
};

// Reader for the pointer-free VITT stream emitted by the Windows oracle.
// Cells retain the source StringTable row-major order, including empty cells,
// so tablelookup actions can use the same reverse-row lookup semantics as the
// retail StringTable_Lookup implementation.
class VitaStringTableBundle {
public:
    bool Load(const VitaAssetDatabase& database);
    void Clear();

    bool Loaded() const { return loaded_; }
    uint32_t Count() const { return static_cast<uint32_t>(records_.size()); }
    const VitaStringTableRecord* Find(const char* name) const;
    const VitaStringTableRecord* Record(uint32_t index) const;
    bool Lookup(const char* name, int comparisonColumn, const char* value,
                int valueColumn, std::string* result) const;

private:
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuCatalogKey catalogKey_;
#endif
    std::vector<VitaStringTableRecord> records_;
    bool loaded_ = false;
};

} // namespace kisak::vita
