#include <vita/vita_script_vm_runtime.h>

#include <psp2/kernel/clib.h>
#include <psp2/io/stat.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <cstring>

#include <universal/q_shared.h>
#include <script/scr_main.h>
#include <script/scr_compiler.h>
#include <script/scr_stringlist.h>
#include <script/scr_variable.h>
#include <script/scr_vm.h>

#include <vita/vita_rawfile_bundle.h>
#include <vita/vita_script_services.h>

namespace kisak::vita {
namespace {

// Keep the native compiler boundary observable on both Vita3K and hardware.
// The visual bootstrap already owns the frame trace, but a parser/ABI fault
// can happen before that writer reaches the renderer.  Open/close per event
// so a crash or forced emulator stop still leaves the last completed stage.
void WriteTraceString(FILE* file, const char* value) {
    std::fputc('"', file);
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(value ? value : "");
         *cursor; ++cursor) {
        switch (*cursor) {
        case '\\': std::fputs("\\\\", file); break;
        case '"': std::fputs("\\\"", file); break;
        case '\n': std::fputs("\\n", file); break;
        case '\r': std::fputs("\\r", file); break;
        case '\t': std::fputs("\\t", file); break;
        default: std::fputc(*cursor, file); break;
        }
    }
    std::fputc('"', file);
}

void TraceScriptEvent(const char* stage, const char* script = nullptr,
                      uint32_t handle = 0, const char* detail = nullptr) {
#if defined(KISAK_VITA_SCRIPT_ASSET_PROBE)
    constexpr const char* kTraceRoot = "ux0:data/CODVIT002";
#else
    constexpr const char* kTraceRoot = "ux0:data/CODVIT001";
#endif
    sceIoMkdir(kTraceRoot, 0777);
    char logsPath[96]{};
    std::snprintf(logsPath, sizeof(logsPath), "%s/logs", kTraceRoot);
    sceIoMkdir(logsPath, 0777);
    char tracePath[128]{};
    std::snprintf(tracePath, sizeof(tracePath), "%s/script-vm.jsonl", logsPath);
    FILE* file = std::fopen(tracePath, "ab");
    if (!file)
        return;
    std::fputs("{\"stage\":", file);
    WriteTraceString(file, stage);
    if (script) {
        std::fputs(",\"script\":", file);
        WriteTraceString(file, script);
    }
    std::fprintf(file, ",\"handle\":%u", static_cast<unsigned>(handle));
    if (detail) {
        std::fputs(",\"detail\":", file);
        WriteTraceString(file, detail);
    }
    std::fputs("}\n", file);
    std::fflush(file);
    std::fclose(file);
}

bool g_loaded = false;
bool g_scriptLoadOpen = false;
bool g_systemInitialized = false;
// The desktop bootstrap owns the process-wide script-string pool before it
// reaches Scr_InitVariables/Scr_Init.  The isolated Vita probe has to make
// that ordering explicit; retain the pool across a failed/repeated load just
// as the normal process does instead of calling SL_Init twice.
bool g_stringListInitialized = false;
uint32_t g_handle = 0;
uint32_t g_callbackHandles[3] = {};

struct NativeHandleSpec {
    const char* filename;
    const char* label;
    uint32_t handle;
};

// Scr_EndLoadScripts releases scrCompilePub.scripts after the compiled code
// positions have been assigned. Cache the native g_scr_data labels while that
// lookup object is alive so post-load entity bootstrap can use the same
// function handles as the desktop GScr_LoadScriptAndLabel path.
NativeHandleSpec g_nativeHandles[] = {
    {"maps/mp/mp_shipment", "main", 0},
    {"maps/mp/gametypes/dm", "main", 0},
    {"maps/mp/gametypes/_callbacksetup", "CodeCallback_StartGameType", 0},
    {"maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerConnect", 0},
    {"maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerDisconnect", 0},
    {"maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerDamage", 0},
    {"maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerKilled", 0},
    {"maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerLastStand", 0},
    {"codescripts/delete", "main", 0},
    {"codescripts/struct", "initstructs", 0},
    {"codescripts/struct", "createstruct", 0},
};

struct ShipmentScriptSpec {
    const char* name;
    const char* label;
};

#if defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
// The compiler stores only relative code offsets after Scr_EndLoadScripts.
// For the experimental probe, recover the authored function name from the
// raw GSC text and print labels near a failing VM position.  This is strictly
// diagnostic; it does not alter the production script load or dispatch path.
void TraceAuthoredFunctionOffsets(const VitaRawFileBundle& rawfiles,
                                  const char* filename,
                                  uint32_t lowerOffset,
                                  uint32_t upperOffset) {
    if (!filename)
        return;
    std::string recordName(filename);
    recordName += ".gsc";
    const VitaRawFileRecord* record = rawfiles.Find(recordName.c_str());
    sceClibPrintf("[vita-script] authored-scan file=%s found=%u bytes=%u\\n",
                  filename, record ? 1u : 0u,
                  record ? static_cast<unsigned>(record->bytes.size()) : 0u);
    if (!record || record->bytes.empty())
        return;
    const std::string_view source(
        reinterpret_cast<const char*>(record->bytes.data()), record->bytes.size());
    size_t lineStart = 0;
    while (lineStart < source.size()) {
        const size_t lineEnd = source.find_first_of("\r\n", lineStart);
        const size_t boundedEnd = lineEnd == std::string_view::npos
            ? source.size() : lineEnd;
        std::string_view line = source.substr(lineStart, boundedEnd - lineStart);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        size_t open = line.find('(');
        if (open != std::string_view::npos && open > 0) {
            size_t nameEnd = open;
            while (nameEnd > 0 && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t'))
                --nameEnd;
            size_t nameBegin = nameEnd;
            while (nameBegin > 0) {
                const char ch = line[nameBegin - 1];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                      || (ch >= '0' && ch <= '9') || ch == '_'))
                    break;
                --nameBegin;
            }
            const std::string_view name = line.substr(nameBegin, nameEnd - nameBegin);
            const bool keyword = name == "if" || name == "for" || name == "while"
                || name == "switch";
            const size_t close = line.find(')', open + 1);
            if (!name.empty() && !keyword && close != std::string_view::npos) {
                bool hasBody = line.find('{', close + 1) != std::string_view::npos;
                if (!hasBody) {
                    size_t next = boundedEnd;
                    while (next < source.size()
                           && (source[next] == '\r' || source[next] == '\n'))
                        ++next;
                    while (next < source.size()
                           && (source[next] == ' ' || source[next] == '\t'))
                        ++next;
                    hasBody = next < source.size() && source[next] == '{';
                }
                if (hasBody) {
                    const std::string label(name);
                    const int handle = Scr_GetFunctionHandle(filename, label.c_str());
                    if (handle >= static_cast<int>(lowerOffset)
                        && handle <= static_cast<int>(upperOffset)) {
                        sceClibPrintf("[vita-script] authored-function=%s::%s offset=%d\n",
                                      filename, label.c_str(), handle);
                    }
                }
            }
        }
        if (lineEnd == std::string_view::npos)
            break;
        lineStart = lineEnd + 1;
        if (lineStart < source.size() && source[lineStart - 1] == '\r'
            && source[lineStart] == '\n')
            ++lineStart;
    }
}

void TraceAllAuthoredFunctionOffsets(const VitaRawFileBundle& rawfiles,
                                     uint32_t lowerOffset,
                                     uint32_t upperOffset) {
    for (uint32_t index = 0; index < rawfiles.Count(); ++index) {
        const VitaRawFileRecord* record = rawfiles.Record(index);
        if (!record || record->name.size() <= 4
            || record->name.compare(record->name.size() - 4, 4, ".gsc") != 0)
            continue;
        const std::string filename = record->name.substr(0, record->name.size() - 4);
        TraceAuthoredFunctionOffsets(rawfiles, filename.c_str(), lowerOffset,
                                     upperOffset);
    }
}
#endif

// Keep the native compiler/VM load set identical to the package closure used
// by VitaScriptRuntime.  Loading only dm/_callbacksetup/_spawnlogic made the
// callback handles real, but left the map bootstrap, art, FX, and _load
// scripts outside the native lifecycle.  The two codescripts entries mirror
// the desktop GScr_LoadScripts handle table used by entity methods.
constexpr ShipmentScriptSpec kShipmentScripts[] = {
    {"codescripts/delete", "codescripts/delete"},
    {"codescripts/struct", "codescripts/struct"},
    {"maps/mp/mp_shipment", "mp_shipment"},
    {"maps/mp/mp_shipment_fx", "mp_shipment_fx"},
    {"maps/createart/mp_shipment_art", "mp_shipment_art"},
    {"maps/mp/_load", "_load"},
    {"maps/mp/gametypes/_spawnlogic", "_spawnlogic"},
    {"maps/mp/gametypes/_callbacksetup", "_callbacksetup"},
    {"maps/mp/gametypes/dm", "dm"},
};

} // namespace

bool VitaScriptVm_LoadShipment(const VitaRawFileBundle& rawfiles) {
#if defined(KISAK_VITA_SCRIPT_ASSET_PROBE)
    VitaScriptServicesSetTraceAppId("CODVIT002");
#else
    VitaScriptServicesSetTraceAppId("CODVIT001");
#endif
    TraceScriptEvent("load_begin", nullptr, 0,
                     rawfiles.Loaded() ? "rawfiles-present" : "rawfiles-missing");
    VitaScriptVm_Shutdown();
    if (!rawfiles.Loaded() || rawfiles.Count() == 0) {
        TraceScriptEvent("load_failed", nullptr, 0, "empty-rawfile-bundle");
        return false;
    }

    VitaScriptServicesBeginRawFiles();
    for (uint32_t index = 0; index < rawfiles.Count(); ++index) {
        const VitaRawFileRecord* record = rawfiles.Record(index);
        if (!record || record->name.empty() || record->bytes.empty())
            continue;
        VitaScriptServicesInstallRawFile(
            record->name.c_str(),
            reinterpret_cast<const char*>(record->bytes.data()),
            record->bytes.size());
    }
    if (!g_stringListInitialized) {
        SL_Init();
        g_stringListInitialized = true;
    }
    Scr_InitVariables();
    Scr_InitClassMap();
    Scr_Init();
    // Match the desktop Com_Init/Com_SetScriptSettings ordering: compiler
    // mode is fixed before the load transaction, while game-system objects
    // are created only after the compiler has closed its script batch.
    Scr_Settings(0, 0, 0);
    Scr_BeginLoadScripts();
    g_scriptLoadOpen = true;
    g_handle = 0;
    bool scriptsLoaded = true;
    for (const ShipmentScriptSpec& script : kShipmentScripts) {
        TraceScriptEvent("script_begin", script.name);
        const uint32_t handle = Scr_LoadScript(script.name);
        sceClibPrintf("[vita-script] load %s handle=%u\n", script.label, handle);
        TraceScriptEvent("script_result", script.name, handle,
                         VitaScriptServicesHadError()
                             ? VitaScriptServicesLastError() : nullptr);
        if (handle == 0)
            scriptsLoaded = false;
        if (std::strcmp(script.name, "maps/mp/gametypes/dm") == 0)
            g_handle = handle;
    }
    // Resolve the same callback handles that the retail g_scr_main_mp path
    // stores before it starts a DM match.  The Vita visual bridge still owns
    // entity/snapshot transport, but callback selection is now gated by the
    // native compiler/VM rather than source-name inspection alone.
    const int spawn = Scr_GetFunctionHandle(
        "maps/mp/gametypes/dm", "onSpawnPlayer");
    const int damage = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerDamage");
    const int killed = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_callbacksetup", "CodeCallback_PlayerKilled");
    g_callbackHandles[0] = spawn > 0 ? static_cast<uint32_t>(spawn) : 0;
    g_callbackHandles[1] = damage > 0 ? static_cast<uint32_t>(damage) : 0;
    g_callbackHandles[2] = killed > 0 ? static_cast<uint32_t>(killed) : 0;
    for (NativeHandleSpec& native : g_nativeHandles) {
        const int handle = Scr_GetFunctionHandle(native.filename, native.label);
        native.handle = handle > 0 ? static_cast<uint32_t>(handle) : 0;
    }
#if defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    // Keep the native probe's entry-point table auditable.  These labels are
    // all compiled into the same program buffer; printing their offsets lets
    // us distinguish a bad callback receiver from a handle resolving to the
    // wrong GSC function without invoking any extra VM code.
    const int globallogicInit = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_globallogic", "init");
    const int callbackSetup = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_callbacksetup", "SetupCallbacks");
    const int tweakablesInit = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_tweakables", "init");
    const int spectatingPermissions = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_spectating", "setSpectatePermissions");
    const int tweakablesRegister = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_tweakables", "registerTweakable");
    const int tweakablesGetValue = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_tweakables", "getTweakableValue");
    const int tweakablesUpdateUi = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_tweakables", "updateUITweakables");
    const int globalSetupCallbacks = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_globallogic", "SetupCallbacks");
    const int globalCallbackPlayerConnect = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_globallogic", "Callback_PlayerConnect");
    const int globalMenuClass = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_globallogic", "menuClass");
    const int globalSpawnClient = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_globallogic", "spawnClient");
    const int globalSetSpawnVariables = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_globallogic", "setSpawnVariables");
    const int killcamInit = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_killcam", "init");
    const int killcamMain = Scr_GetFunctionHandle(
        "maps/mp/gametypes/_killcam", "killcam");
    sceClibPrintf("[vita-script] entry-offsets dm=%d globallogic=%d callback=%d "
                  "tweakables=%d spectating=%d register=%d getvalue=%d "
                  "updateui=%d globalsetup=%d connect=%d menuclass=%d "
                  "spawnclient=%d setspawn=%d killcaminit=%d killcam=%d\\n",
                  g_nativeHandles[1].handle, globallogicInit, callbackSetup,
                  tweakablesInit, spectatingPermissions, tweakablesRegister,
                  tweakablesGetValue, tweakablesUpdateUi, globalSetupCallbacks,
                  globalCallbackPlayerConnect, globalMenuClass, globalSpawnClient,
                  globalSetSpawnVariables, killcamInit, killcamMain);
    TraceAuthoredFunctionOffsets(rawfiles, "maps/mp/gametypes/_globallogic",
                                 0u, 200000u);
    TraceAuthoredFunctionOffsets(rawfiles, "maps/mp/gametypes/_tweakables",
                                 0u, 200000u);
    TraceAuthoredFunctionOffsets(rawfiles, "maps/mp/gametypes/_spectating",
                                 0u, 200000u);
    TraceAllAuthoredFunctionOffsets(rawfiles, 5000u, 7000u);
    TraceAllAuthoredFunctionOffsets(rawfiles, 129000u, 131000u);
#endif
    TraceScriptEvent("callbacks_resolved", "dm", g_handle,
                     VitaScriptServicesHadError()
                         ? VitaScriptServicesLastError() : nullptr);
    if (!scriptsLoaded || g_handle == 0 || !g_callbackHandles[0] || !g_callbackHandles[1]
        || !g_callbackHandles[2] || VitaScriptServicesHadError()) {
        sceClibPrintf("[vita-script] Shipment VM load failed handle=%u error=%s\n",
                      g_handle, VitaScriptServicesLastError());
        if (g_scriptLoadOpen) {
            Scr_EndLoadScripts();
            g_scriptLoadOpen = false;
        }
        g_handle = 0;
        for (uint32_t& callback : g_callbackHandles)
            callback = 0;
        for (NativeHandleSpec& native : g_nativeHandles)
            native.handle = 0;
        VitaScriptServicesClearRawFile();
        TraceScriptEvent("load_failed", nullptr, 0,
                         VitaScriptServicesLastError());
        return false;
    }
    TraceScriptEvent("post_compile_begin");
    Scr_PostCompileScripts();
    Scr_EndLoadScripts();
    g_scriptLoadOpen = false;
    Scr_InitSystem(1);
    g_systemInitialized = true;
    g_loaded = !VitaScriptServicesHadError();
    TraceScriptEvent(g_loaded ? "ready" : "load_failed", nullptr, g_handle,
                     VitaScriptServicesHadError()
                         ? VitaScriptServicesLastError() : nullptr);
    sceClibPrintf("[vita-script] Shipment VM loaded handle=%u callbacks=%u/%u/%u rawfiles=%u\n",
                  g_handle, g_callbackHandles[0], g_callbackHandles[1],
                  g_callbackHandles[2], rawfiles.Count());
#if defined(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    if (scrVarPub.programBuffer && scrCompilePub.programLen > 65650u) {
        for (uint32_t offset = 65280u; offset < 65650u; offset += 16u) {
            char bytes[192];
            int written = std::snprintf(bytes, sizeof(bytes),
                                         "[vita-script] vm-bytes=%u:", offset);
            for (uint32_t index = 0; index < 16u && written > 0 &&
                 static_cast<size_t>(written) < sizeof(bytes); ++index) {
                written += std::snprintf(bytes + written, sizeof(bytes) - written,
                                         " %02x", static_cast<unsigned>(static_cast<unsigned char>(scrVarPub.programBuffer[offset + index])));
            }
            sceClibPrintf("%s\\n", bytes);
        }
    }
#endif
    if (!g_loaded) {
        if (g_systemInitialized) {
            Scr_ShutdownSystem(1, 1);
            g_systemInitialized = false;
        }
        g_handle = 0;
        for (uint32_t& callback : g_callbackHandles)
            callback = 0;
        for (NativeHandleSpec& native : g_nativeHandles)
            native.handle = 0;
        VitaScriptServicesClearRawFile();
    }
    return g_loaded;
}

void VitaScriptVm_Shutdown() {
    TraceScriptEvent("shutdown", nullptr, g_handle);
    VitaScriptVm_ShutdownEntityRuntime();
    if (g_scriptLoadOpen) {
        Scr_EndLoadScripts();
        g_scriptLoadOpen = false;
    }
    if (g_systemInitialized) {
        Scr_ShutdownSystem(1, 1);
        g_systemInitialized = false;
    }
    g_loaded = false;
    g_handle = 0;
    for (uint32_t& callback : g_callbackHandles)
        callback = 0;
    for (NativeHandleSpec& native : g_nativeHandles)
        native.handle = 0;
    VitaScriptServicesClearRawFile();
}

bool VitaScriptVm_Loaded() { return g_loaded; }
uint32_t VitaScriptVm_Handle() { return g_handle; }

uint32_t VitaScriptVm_NativeFunctionHandle(const char* filename,
                                           const char* label) {
    if (!filename || !label)
        return 0;
    for (const NativeHandleSpec& native : g_nativeHandles) {
        if (std::strcmp(native.filename, filename) == 0
            && std::strcmp(native.label, label) == 0)
            return native.handle;
    }
    return 0;
}

uint32_t VitaScriptVm_CallbackHandle(uint8_t eventType) {
    switch (eventType) {
    case 0: // spawn/respawn
    case 3:
        return g_callbackHandles[0];
    case 4: // damage
        return g_callbackHandles[1];
    case 5: // killed
        return g_callbackHandles[2];
    default:
        return 0;
    }
}

bool VitaScriptVm_HasCallback(uint8_t eventType) {
    return g_loaded && VitaScriptVm_CallbackHandle(eventType) != 0;
}


} // namespace kisak::vita
