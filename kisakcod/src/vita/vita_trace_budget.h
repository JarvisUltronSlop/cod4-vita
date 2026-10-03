#pragma once
#include <cstddef>
#include <cstring>

namespace kisak::vita {
// Separate bounded reserves keep later map failures observable after parser
// diagnostics exhaust their quota. No allocations or disk activity here.
struct VitaTraceBudget {
    static constexpr std::size_t kRegular=4096, kNoisy=128, kLifecycle=512, kFailure=64;
    std::size_t regular=0, noisy=0, lifecycle=0, failure=0;
    bool Admit(const char* stage, const char* subject, bool isNoisy) {
        const auto failureText=[](const char* text) {
            return text && (std::strstr(text,"fail") || std::strstr(text,"error"));
        };
        if (failureText(stage) || (stage && !std::strcmp(stage,"native_stage") && failureText(subject)))
            return Take(failure,kFailure);
        if (stage && (!std::strncmp(stage,"map_",4)
            || !std::strcmp(stage,"source_registry_stage")
            || !std::strcmp(stage,"native_stage")
            || !std::strcmp(stage,"vita_package_probe")
            || !std::strcmp(stage,"weapon_memory_release")))
            return Take(lifecycle,kLifecycle);
        return isNoisy ? Take(noisy,kNoisy) : Take(regular,kRegular);
    }
private:
    static bool Take(std::size_t& used, std::size_t limit) {
        if (used>=limit) return false;
        ++used; return true;
    }
};
}
