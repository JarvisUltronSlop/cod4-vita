#include <cstdio>
#include <cstdint>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include <vita/kvap_reader.h>
#include <vita/vita_asset_db.h>
#include <vita/vita_rawfile_bundle.h>
#include <vita/vita_script_runtime.h>
#include <vita/vita_script_vm_runtime.h>

namespace {

struct FileContext {
    FILE* file = nullptr;
};

bool ReadFile(void* opaque, uint64_t offset, void* destination, size_t size) {
    auto* context = static_cast<FileContext*>(opaque);
    if (!context || !context->file || !destination)
        return false;
    if (fseek(context->file, static_cast<long>(offset), SEEK_SET) != 0)
        return false;
    return std::fread(destination, 1, size, context->file) == size;
}

bool ProbePackage(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        sceClibPrintf("[script-asset-probe] missing=%s\n", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long end = ftell(file);
    if (end <= 0) {
        std::fclose(file);
        return false;
    }
    FileContext context{file};
    kisak::vita::VitaAssetDatabase database;
    const auto mount = database.Mount(ReadFile, &context,
                                      static_cast<uint64_t>(end));
    sceClibPrintf("[script-asset-probe] package=%s mount=%u chunks=%u\n",
                  path, static_cast<unsigned>(mount), database.ChunkCount());
    kisak::vita::VitaRawFileBundle rawfiles;
    const bool rawLoaded = mount == kisak::vita::KvapError::Ok
        && rawfiles.Load(database);
    sceClibPrintf("[script-asset-probe] rawfiles=%u records=%u\n",
                  static_cast<unsigned>(rawLoaded), rawfiles.Count());
    kisak::vita::VitaScriptRuntime scriptRuntime;
    const bool rulesLoaded = rawLoaded && scriptRuntime.Load(rawfiles)
        && scriptRuntime.DeathmatchRulesLoaded();
    if (rulesLoaded) {
        const auto& rules = scriptRuntime.DeathmatchRules();
        sceClibPrintf("[script-asset-probe] dm_rules=%u timelimit=%d scorelimit=%d kill=%d teamBased=%u\n",
                      static_cast<unsigned>(rulesLoaded), rules.timeLimitMsec,
                      rules.scoreLimit, rules.killScore,
                      static_cast<unsigned>(rules.teamBased));
    } else {
        sceClibPrintf("[script-asset-probe] dm_rules=0\n");
    }
    const bool vmLoaded = rawLoaded
        && kisak::vita::VitaScriptVm_LoadShipment(rawfiles);
    const bool entityReady = vmLoaded
        && kisak::vita::VitaScriptVm_InitializeEntityRuntime();
    sceClibPrintf("[script-asset-probe] vm=%u handle=%u callbacks=%u/%u/%u\n",
                  static_cast<unsigned>(vmLoaded),
                  kisak::vita::VitaScriptVm_Handle(),
                  kisak::vita::VitaScriptVm_CallbackHandle(0),
                  kisak::vita::VitaScriptVm_CallbackHandle(4),
                  kisak::vita::VitaScriptVm_CallbackHandle(5));
    sceClibPrintf("[script-asset-probe] entity=%u\n",
                  static_cast<unsigned>(entityReady));
    kisak::vita::VitaScriptVm_Shutdown();
    database.Unmount();
    std::fclose(file);
    return vmLoaded && entityReady && rulesLoaded;
}

} // namespace

int main() {
    sceClibPrintf("[script-asset-probe] start\n");
    const char* const paths[] = {
        "app0:data/shipment-db-reference.kvap",
        // The standalone probe is packaged as CODVIT002. Prefer its external
        // data route when app0 is unavailable so a probe cannot silently read
        // the production bootstrap's CODVIT001 data directory.
        "ux0:data/CODVIT002/assets/shipment-db-reference.kvap",
        // Keep the production route as a final compatibility fallback for
        // older installs that staged the probe package beside CODVIT001.
        "ux0:data/CODVIT001/assets/shipment-db-reference.kvap",
    };
    for (const char* path : paths) {
        if (ProbePackage(path)) {
            sceClibPrintf("[script-asset-probe] success\n");
            return 0;
        }
    }
    sceClibPrintf("[script-asset-probe] no package or VM failure\n");
    return 2;
}
