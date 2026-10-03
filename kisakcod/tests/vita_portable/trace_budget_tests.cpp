#include <vita/vita_trace_budget.h>
#include <cstdio>
#include <cstdlib>
using kisak::vita::VitaTraceBudget;
static void Check(bool value) { if (!value) std::abort(); }
int main() {
    VitaTraceBudget budget;
    for (unsigned i=0;i<VitaTraceBudget::kRegular;++i)
        Check(budget.Admit("script_compile_begin","dm",false));
    Check(!budget.Admit("script_compile_end","dm",false));
    // Parser quota was exhausted before reload 5 in bench 9.
    for (unsigned i=0;i<5;++i) {
        Check(budget.Admit("map_reload_checkpoint","WORLD GEOMETRY",false));
        Check(budget.Admit("source_registry_stage","models",false));
    }
    while (budget.lifecycle<VitaTraceBudget::kLifecycle)
        Check(budget.Admit("map_load_begin","mp_shipment",false));
    Check(!budget.Admit("map_load_begin","mp_shipment",false));
    Check(budget.Admit("map_load_semantic_residency_failed","mp_shipment",false));
    Check(budget.Admit("native_stage","world_archive_failed",false));
    Check(budget.Admit("source_error","allocation failed",false));
    while (budget.failure<VitaTraceBudget::kFailure)
        Check(budget.Admit("map_reload_failure","HUD RESOURCES",false));
    Check(!budget.Admit("map_reload_failure","HUD RESOURCES",false));
    for (unsigned i=0;i<VitaTraceBudget::kNoisy;++i)
        Check(budget.Admit("pmove_enter",nullptr,true));
    Check(!budget.Admit("pmove_enter",nullptr,true));
    budget={}; Check(budget.Admit("script_compile_begin",nullptr,false));
    Check(budget.lifecycle==0 && budget.failure==0);
    std::puts("trace reserves remain available and bounded");
}
