#pragma once
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <Windows.h>

// Only the explicitly requested, local asset-export process uses this mode.
// It keeps the source database/renderer available without foreground windows,
// OS input, or a Steam session while another application is in use.
inline bool Win_IsHeadlessVitaExport() {
    static const bool enabled = [] {
        const char* exportMode = std::getenv("KISAK_VITA_EXPORT");
        const char* headless = std::getenv("KISAK_VITA_EXPORT_HEADLESS");
        return exportMode && headless && std::strcmp(exportMode, "1") == 0
            && std::strcmp(headless, "1") == 0;
    }();
    return enabled;
}

inline int Win_VitaExportMessageBox(HWND owner, const char* text,
    const char* caption, unsigned int flags) {
    if (Win_IsHeadlessVitaExport()) {
        std::fprintf(stderr, "[KisakCOD export error] %s: %s\n",
            caption ? caption : "", text ? text : "");
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }
    return MessageBoxA(owner, text, caption, flags);
}
