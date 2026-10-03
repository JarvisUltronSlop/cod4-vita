#pragma once

#include <cstddef>
#include <csetjmp>
#include <cstdint>

namespace kisak::vita {

// Starts a bounded package-owned rawfile mount for the real GSC loader. The
// caller retains ownership of the names/bytes for the duration of the script
// load; no filesystem path is consulted on Vita. InstallRawFile remains a
// single-file convenience for small probes; callers mounting a dependency
// closure should call BeginRawFiles first and then install every record.
void VitaScriptServicesBeginRawFiles();
bool VitaScriptServicesInstallRawFile(const char* name, const char* bytes,
                                      std::size_t length);
void VitaScriptServicesClearRawFile();
void VitaScriptServicesReleaseHunk();
void VitaScriptServicesReleaseWeaponInfo();

// The GSC loader is shared by the bootstrap and the renderer-free probe. Set
// the title-id before loading so source/parse/compile markers land beside the
// wrapper VM trace for the process that produced them.
void VitaScriptServicesSetTraceAppId(const char* titleId);
void VitaScriptServicesTrace(const char* stage, const char* subject = nullptr,
                             const char* detail = nullptr);

bool VitaScriptServicesHadError();
const char* VitaScriptServicesLastError();
void VitaScriptServicesClearError();
// Observe errors after the original VM has entered its error handler. The VM
// still unwinds its own stack; the owning session fails at its next boundary.
void VitaScriptServicesRecordRuntimeError(const char* message);
uint64_t VitaScriptServicesRuntimeErrorCount();

// Com_Error/Sys_Error are non-returning in the original engine. Source code
// routinely assumes that contract, so a Vita session must install one of
// these boundaries before invoking an engine root. A raised error returns to
// the active boundary instead of continuing through partially initialized
// source state. Boundaries are main-thread only and may be nested.
struct VitaSourceErrorBoundary {
    std::jmp_buf jump;
    VitaSourceErrorBoundary* previous = nullptr;
};

void VitaScriptServicesPushErrorBoundary(VitaSourceErrorBoundary* boundary);
void VitaScriptServicesPopErrorBoundary(VitaSourceErrorBoundary* boundary);

} // namespace kisak::vita
