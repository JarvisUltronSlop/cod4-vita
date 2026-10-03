#include <universal/q_shared.h>
#include <script/scr_main.h>
#include <script/scr_vm.h>
#include <script/scr_variable.h>
#include <vita/vita_script_services.h>
#include <psp2/kernel/clib.h>

// Keep this probe source-backed so every change to the rawfile/VM boundary
// forces the focused ARM link gate to rerun.
namespace {

constexpr char kProbeScript[] =
    "main() {\n"
    "    return;\n"
    "}\n";

} // namespace

// Link probe for the real compiler/VM entry points.  This deliberately does
// not enter the bootstrap until the Vita storage and builtin-service boundary
// is supplied; its unresolved set is the implementation worklist.
int main() {
    sceClibPrintf("[script-probe] start\\n");
    kisak::vita::VitaScriptServicesInstallRawFile(
        "maps/mp/gametypes/dm.gsc", kProbeScript, sizeof(kProbeScript) - 1);
    Scr_InitVariables();
    sceClibPrintf("[script-probe] variables\\n");
    Scr_InitClassMap();
    Scr_Init();
    sceClibPrintf("[script-probe] vm\\n");
    Scr_InitSystem(1);
    sceClibPrintf("[script-probe] system\\n");
    Scr_BeginLoadScripts();
    sceClibPrintf("[script-probe] begin\\n");
    const uint32_t handle = Scr_LoadScript("maps/mp/gametypes/dm");
    sceClibPrintf("[script-probe] loaded=%u\\n", handle);
    Scr_PostCompileScripts();
    Scr_EndLoadScripts();
    sceClibPrintf("[script-probe] end\\n");
    Scr_ShutdownSystem(1, 1);
    const bool failed = handle == 0 || kisak::vita::VitaScriptServicesHadError();
    kisak::vita::VitaScriptServicesClearRawFile();
    return failed ? 2 : 0;
}
