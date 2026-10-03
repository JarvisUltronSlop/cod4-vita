#include <vita/vita_script_services.h>
#include <vita/vita_cpu_profile.h>
#include <vita/vita_source_platform.h>
#include <vita/vita_hunk_arena.h>
#include <vita/vita_temp_hunk.h>
#include <vita/vita_trace_budget.h>

#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <universal/q_shared.h>
#include <database/database.h>
#include <client_mp/client_mp.h>
#include <qcommon/qcommon.h>
#include <server_mp/server_mp.h>
#include <script/scr_debugger.h>
#include <script/scr_vm.h>
#include <universal/com_files.h>
#include <universal/com_memory.h>
#include <universal/com_sndalias.h>
#include <universal/profile.h>
#include <universal/q_parse.h>
#include <sound/snd_public.h>
#include <physics/phys_local.h>
#include <win32/win_net_debug.h>
#include <xanim/xanim.h>

#if defined(__GNUC__)
#define KISAK_VITA_WEAK __attribute__((weak))
#else
#define KISAK_VITA_WEAK
#endif

namespace {

constexpr std::size_t kMaxScriptBytes = 32u * 1024u * 1024u;
constexpr std::size_t kMaxMountedRawFiles = 1024;
constexpr std::size_t kMaxOpenRawFiles = 32;
// Typed assets and GPU resources have separate lifetimes. The runtime hunk
// commits stable pages on demand, bounded at 16 MiB, rather than requiring
// a large contiguous reservation before the first source allocation.
constexpr std::size_t kVitaHunkCapacity = kisak::vita::VitaHunkArena::kCapacity;
kisak::vita::VitaHunkArena g_sourceHunk;
constexpr std::size_t kMaxVitaFileData = 1024;
constexpr std::size_t kLargeLocalCapacity = 0x80000u;
// Script/parser tracing is valuable while closing the ARM boundary, but the
// lexer and VM can emit thousands of progress events during one boot. Keep
// the diagnostic stream bounded and reuse one open handle; reopening an
// append-only file for every event can dominate Vita3K's scalar path and grow
// a stale log into hundreds of megabytes across repeated smoke runs.
// Event quotas are separately reserved for parser, lifecycle and failure logs.

struct MountedRawFile {
    const char* name = nullptr;
    const char* bytes = nullptr;
    // Fastfile script loading treats RawFile::buffer as a C string and calls
    // strlen() before handing it to the parser.  VitaRawFileBundle records
    // are deliberately length-delimited, so keep an owned, NUL-terminated
    // view at this platform boundary instead of reading past the package
    // payload.  raw.len remains the original payload length.
    char* ownedBytes = nullptr;
    std::size_t length = 0;
    RawFile raw = {};
};

struct OpenRawFile {
    bool used = false;
    int handle = 0;
    std::size_t record = 0;
    std::size_t offset = 0;
    FILE* stream = nullptr;
};

MountedRawFile g_rawFiles[kMaxMountedRawFiles] = {};
std::size_t g_rawFileCount = 0;
OpenRawFile g_openRawFiles[kMaxOpenRawFiles] = {};
bool g_rawFileBatch = false;
bool g_scriptError = false;
uint64_t g_runtimeScriptErrors = 0;
char g_lastError[256] = {};
kisak::vita::VitaSourceErrorBoundary* g_errorBoundary = nullptr;
char g_traceRoot[32] = "ux0:data/CODVIT001";
FILE* g_traceFile = nullptr;
kisak::vita::VitaTraceBudget g_traceBudget;

HunkUser* g_tempUser = nullptr;
kisak::vita::VitaTempHunk<> g_tempHigh;

struct VitaFileDataRecord {
    bool used;
    int type;
    void* data;
    char name[256];
};

VitaFileDataRecord g_fileData[kMaxVitaFileData] = {};
unsigned char* g_largeLocalBuf = nullptr;
std::size_t g_largeLocalPos = 0;
std::size_t g_hunkPos = 0;
std::size_t g_hunkTraceCount = 0;
std::size_t g_debugHunkTraceCount = 0;

dvar_t g_useFastFile = {};
dvar_t g_gameDir = {};

uintptr_t AlignUp(uintptr_t value, uintptr_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void RecordError(const char* fmt, va_list args) {
    g_scriptError = true;
    std::vsnprintf(g_lastError, sizeof(g_lastError), fmt, args);
    g_lastError[sizeof(g_lastError) - 1] = '\0';
    std::fprintf(stderr, "[vita-script] %s\n", g_lastError);
    sceClibPrintf("[kisak-source-error] %s\n", g_lastError);
    kisak::vita::VitaScriptServicesTrace("source_error", g_lastError);
}

void RecordError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    RecordError(fmt, args);
    va_end(args);
}

bool RawPathEqual(const char* left, const char* right) {
    if (!left || !right)
        return false;
    while (*left || *right) {
        char a = *left++;
        char b = *right++;
        if (a == '\\')
            a = '/';
        if (b == '\\')
            b = '/';
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return true;
}

MountedRawFile* FindMountedRawFile(const char* name) {
    if (!name)
        return nullptr;
    for (std::size_t index = 0; index < g_rawFileCount; ++index) {
        MountedRawFile& record = g_rawFiles[index];
        if (record.name && RawPathEqual(name, record.name))
            return &record;
    }
    return nullptr;
}

OpenRawFile* FindOpenRawFile(int handle) {
    for (OpenRawFile& file : g_openRawFiles) {
        if (file.used && file.handle == handle)
            return &file;
    }
    return nullptr;
}

const char* RawFileBytes(const MountedRawFile* file) {
    return file && file->bytes && file->length <= kMaxScriptBytes
        ? file->bytes : nullptr;
}

void FreeHunkChain(HunkUser* user) {
    while (user) {
        HunkUser* next = user->next;
        std::free(user);
        user = next;
    }
}

} // namespace

// The desktop hunk allocator reserves virtual address space and uses several
// Win32 commit/decommit paths.  Vita's slice keeps the same public contracts
// behind one bounded, pointer-stable arena plus explicitly tracked temporary
// allocations.  This is intentionally finite so asset growth fails locally
// instead of silently turning into an unbounded process allocation.
unsigned char* s_hunkData = nullptr;
uint8_t* s_origHunkData = nullptr;
int s_hunkTotal = 0;

namespace {

void* VitaHunkAllocInternal(uint32_t size, int alignment) {
    if (alignment <= 0 || alignment > HUNK_MAX_ALIGNEMT
        || (alignment & (alignment - 1)) != 0)
        return nullptr;
    void* allocation = g_sourceHunk.Allocate(size, static_cast<size_t>(alignment));
    if (!allocation) {
        Com_Error(ERR_DROP,
                  "Vita source hunk allocation failed (%u requested, %u used, %u committed, %u limit)",
                  size,
                  static_cast<unsigned>(g_hunkPos),
                  static_cast<unsigned>(g_sourceHunk.Reserved()),
                  static_cast<unsigned>(kVitaHunkCapacity));
        return nullptr;
    }
    g_hunkPos = g_sourceHunk.Mark();
    s_hunkData = s_origHunkData = g_sourceHunk.Base();
    s_hunkTotal = static_cast<int>(g_sourceHunk.Reserved());
    return allocation;
}

bool VitaStringEqual(const char* left, const char* right) {
    if (!left || !right)
        return left == right;
    while (*left && *right) {
        const unsigned char a = static_cast<unsigned char>(*left++);
        const unsigned char b = static_cast<unsigned char>(*right++);
        const unsigned char lowerA = (a >= 'A' && a <= 'Z') ? static_cast<unsigned char>(a + ('a' - 'A')) : a;
        const unsigned char lowerB = (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b + ('a' - 'A')) : b;
        if (lowerA != lowerB)
            return false;
    }
    return *left == *right;
}

VitaFileDataRecord* FindVitaFileData(int type, const char* name) {
    for (auto& record : g_fileData) {
        if (record.used && record.type == type && VitaStringEqual(record.name, name))
            return &record;
    }
    return nullptr;
}

VitaFileDataRecord* NewVitaFileData() {
    for (auto& record : g_fileData) {
        if (!record.used) {
            record = {};
            record.used = true;
            return &record;
        }
    }
    return nullptr;
}

void* AllocateSourceTemp(int size, const char* name) {
    return g_tempHigh.Allocate(size, [=](const char* reason) {
        const auto heap = mallinfo();
        Com_Error(ERR_DROP,
            "source temp hunk %s: %s size=%d allocations=%u heap=%u free=%u",
            reason, name ? name : "unnamed", size,
            static_cast<unsigned>(g_tempHigh.Count()),
            static_cast<unsigned>(heap.uordblks),
            static_cast<unsigned>(heap.fordblks));
    });
}

} // namespace

namespace kisak::vita {

void VitaScriptServicesSetTraceAppId(const char* titleId) {
    if (!titleId || !titleId[0])
        return;
    const std::size_t length = std::strlen(titleId);
    if (length >= sizeof(g_traceRoot) - 9u)
        return;
    if (g_traceFile) {
        std::fclose(g_traceFile);
        g_traceFile = nullptr;
    }
    g_traceBudget = {};
    std::snprintf(g_traceRoot, sizeof(g_traceRoot), "ux0:data/%s", titleId);
}

namespace {

void WriteTraceJsonString(FILE* file, const char* value) {
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

} // namespace

namespace {

bool IsNoisyTraceStage(const char* stage) {
    if (!stage)
        return false;
    return std::strstr(stage, "_progress") != nullptr
        || std::strncmp(stage, "mt_", 3) == 0
        || std::strcmp(stage, "string_begin") == 0
        || std::strcmp(stage, "string_end") == 0
        || std::strcmp(stage, "calc_local") == 0
        || std::strcmp(stage, "formal_param") == 0
        || std::strcmp(stage, "pmove_enter") == 0
        || std::strcmp(stage, "pmove_result") == 0
        || std::strcmp(stage, "pmove_exit") == 0
        || std::strcmp(stage, "trace_result") == 0
        || std::strcmp(stage, "trace_capsule") == 0
        || std::strcmp(stage, "vita_input_sample") == 0
        || std::strcmp(stage, "vita_input_analog") == 0;
}

} // namespace

void VitaScriptServicesTrace(const char* stage, const char* subject,
                             const char* detail) {
    if (!stage || !stage[0])
        return;
#if !KISAK_VITA_PROFILE
    if (IsNoisyTraceStage(stage) || std::strncmp(stage, "pmove_", 6) == 0
        || std::strncmp(stage, "pm_ground_", 10) == 0)
        return;
#endif
    if (!g_traceBudget.Admit(stage, subject, IsNoisyTraceStage(stage)))
        return;
#if KISAK_VITA_CPU_RUNTIME
    VitaCpuProfile_Trace(stage,subject,detail);
    return;
#endif
    if (!g_traceFile) {
        sceIoMkdir(g_traceRoot, 0777);
        char logsPath[64]{};
        std::snprintf(logsPath, sizeof(logsPath), "%s/logs", g_traceRoot);
        sceIoMkdir(logsPath, 0777);
        char tracePath[96]{};
        std::snprintf(tracePath, sizeof(tracePath), "%s/script-stages.jsonl", logsPath);
        // Each process owns its trace. This removes stale multi-run growth and
        // makes the file a reliable record of the current boot attempt.
        std::remove(tracePath);
        g_traceFile = std::fopen(tracePath, "wb");
        if (!g_traceFile)
            return;
    }
    FILE* file = g_traceFile;
    std::fputs("{\"stage\":", file);
    WriteTraceJsonString(file, stage);
    if (subject) {
        std::fputs(",\"subject\":", file);
        WriteTraceJsonString(file, subject);
    }
    if (detail) {
        std::fputs(",\"detail\":", file);
        WriteTraceJsonString(file, detail);
    }
    std::fputs("}\n", file);
    std::fflush(file);
}

void VitaScriptServicesBeginRawFiles() {
    VitaScriptServicesClearRawFile();
    g_rawFileBatch = true;
}

bool VitaScriptServicesInstallRawFile(const char* name, const char* bytes,
                                      std::size_t length) {
    if (!g_rawFileBatch)
        VitaScriptServicesBeginRawFiles();
    if (!name || name[0] == '\0' || (!bytes && length != 0)
        || length > kMaxScriptBytes) {
        RecordError("invalid rawfile mount request (%s, %zu bytes)",
                    name ? name : "<null>", length);
        return false;
    }
    if (g_rawFileCount >= kMaxMountedRawFiles) {
        RecordError("rawfile mount exceeds bounded capacity (%zu)",
                    kMaxMountedRawFiles);
        return false;
    }
    if (FindMountedRawFile(name)) {
        RecordError("duplicate rawfile in source closure: %s", name);
        return false;
    }
    char* ownedBytes = static_cast<char*>(std::malloc(length + 1u));
    if (!ownedBytes) {
        RecordError("unable to mount raw script %s (%zu bytes)", name, length);
        return false;
    }
    if (length)
        std::memcpy(ownedBytes, bytes, length);
    ownedBytes[length] = '\0';
    MountedRawFile& record = g_rawFiles[g_rawFileCount++];
    record.name = name;
    record.bytes = ownedBytes;
    record.ownedBytes = ownedBytes;
    record.length = length;
    record.raw.name = record.name;
    record.raw.len = static_cast<int>(length);
    record.raw.buffer = record.bytes;
    g_useFastFile.current.enabled = true;
    if (!useFastFile) useFastFile = &g_useFastFile; // isolated script probe only
    g_gameDir.current.string = "";
    g_scriptError = false;
    g_lastError[0] = '\0';
    return true;
}

void VitaScriptServicesClearRawFile() {
    g_tempHigh.Clear();
    for (OpenRawFile& file : g_openRawFiles) {
        if (file.stream) std::fclose(file.stream);
        file = {};
    }
    for (MountedRawFile& file : g_rawFiles) {
        std::free(file.ownedBytes);
        file = {};
    }
    g_rawFileCount = 0;
    g_rawFileBatch = false;
    g_largeLocalPos = 0;
    Hunk_ClearData();
}

void VitaScriptServicesReleaseHunk() {
    // Called only after source DObj, XAnim and script teardown. Their pointers
    // must remain valid while they release references into the hunk.
    g_sourceHunk.Clear();
    g_hunkPos = 0;
    s_hunkData = s_origHunkData = nullptr;
    s_hunkTotal = 0;
    g_hunkTraceCount = 0;
}

bool VitaScriptServicesHadError() { return g_scriptError; }

const char* VitaScriptServicesLastError() {
    return g_lastError[0] ? g_lastError : "";
}

void VitaScriptServicesClearError() {
    g_scriptError = false;
    g_lastError[0] = '\0';
}

void VitaScriptServicesRecordRuntimeError(const char* message) {
    if (g_runtimeScriptErrors != UINT64_MAX)
        ++g_runtimeScriptErrors;
    if (!g_scriptError)
        RecordError("script runtime error: %s", message ? message : "unknown");
}

uint64_t VitaScriptServicesRuntimeErrorCount() { return g_runtimeScriptErrors; }

void VitaScriptServicesPushErrorBoundary(VitaSourceErrorBoundary* boundary) {
    if (!boundary)
        return;
    boundary->previous = g_errorBoundary;
    g_errorBoundary = boundary;
}

void VitaScriptServicesPopErrorBoundary(VitaSourceErrorBoundary* boundary) {
    if (!boundary)
        return;
    if (g_errorBoundary == boundary)
        g_errorBoundary = boundary->previous;
    boundary->previous = nullptr;
}

} // namespace kisak::vita

// The script compiler's hunk API is a bounded, pointer-preserving arena on
// Vita. It intentionally does not fall back to the process allocator after a
// failed fixed arena allocation.
HunkUser* __cdecl Hunk_UserCreate(int maxSize, const char* name, bool fixed, bool tempMem, int type) {
    if (maxSize <= 0 || maxSize > 64 * 1024 * 1024) {
        RecordError("invalid source HunkUser request %s (%d bytes)",
                    name ? name : "<unnamed>", maxSize);
        return nullptr;
    }
    const std::size_t bytes = sizeof(HunkUser) + static_cast<std::size_t>(maxSize) + 64u;
    auto* user = static_cast<HunkUser*>(std::calloc(1, bytes));
    if (!user) {
        RecordError("source HunkUser allocation failed %s (%u bytes)",
                    name ? name : "<unnamed>",
                    static_cast<unsigned>(bytes));
        sceClibPrintf("[kisak-vita] source HunkUser failed name=%s bytes=%u\n",
                      name ? name : "<unnamed>",
                      static_cast<unsigned>(bytes));
        return nullptr;
    }
    const uintptr_t first = AlignUp(reinterpret_cast<uintptr_t>(user->buf), 32u);
    user->current = user;
    user->next = nullptr;
    user->maxSize = maxSize;
    user->end = static_cast<int>(first + static_cast<uintptr_t>(maxSize));
    user->pos = static_cast<int>(first);
    user->name = name;
    user->fixed = fixed;
    user->tempMem = tempMem;
    user->type = type;
    sceClibPrintf("[kisak-vita] source HunkUser ready name=%s bytes=%u ptr=%p\n",
                  name ? name : "<unnamed>",
                  static_cast<unsigned>(bytes), static_cast<void*>(user));
    return user;
}

void* __cdecl Hunk_UserAlloc(HunkUser* user, uint32_t size, int alignment) {
    if (!user || alignment <= 0 || (alignment & (alignment - 1)) != 0 || alignment > 4096) {
        return nullptr;
    }
    HunkUser* current = user->current;
    if (!current) {
        return nullptr;
    }
    uintptr_t aligned = AlignUp(static_cast<uintptr_t>(current->pos), static_cast<uintptr_t>(alignment));
    if (aligned + size > static_cast<uintptr_t>(current->end)) {
        if (user->fixed) {
            RecordError("bounded script hunk arena exhausted (%u bytes)", size);
            return nullptr;
        }
        HunkUser* next = Hunk_UserCreate(user->maxSize, user->name, false, user->tempMem, user->type);
        if (!next) {
            return nullptr;
        }
        current->next = next;
        user->current = next;
        current = next;
        // The first alignment was computed against the exhausted segment.
        // Recompute it from the new segment before advancing its position;
        // otherwise a non-fixed rollover returns a pointer into the old
        // arena and corrupts temporary compiler allocations.
        aligned = AlignUp(static_cast<uintptr_t>(current->pos),
                          static_cast<uintptr_t>(alignment));
    }
    current->pos = static_cast<int>(aligned + size);
    return reinterpret_cast<void*>(aligned);
}

void* __cdecl Hunk_UserAllocAlignStrict(HunkUser* user, uint32_t size) {
    return Hunk_UserAlloc(user, size, 1);
}

void __cdecl Hunk_UserSetPos(HunkUser* user, uint8_t* pos) {
    if (!user || !pos || reinterpret_cast<uintptr_t>(pos) < reinterpret_cast<uintptr_t>(user->buf)
        || reinterpret_cast<uintptr_t>(pos) > static_cast<uintptr_t>(user->current->pos))
        return;
    user->current->pos = static_cast<int>(reinterpret_cast<uintptr_t>(pos));
}

void __cdecl Hunk_UserReset(HunkUser* user) {
    if (!user)
        return;
    HunkUser* next = user->next;
    user->next = nullptr;
    user->current = user;
    FreeHunkChain(next);
    user->pos = static_cast<int>(AlignUp(reinterpret_cast<uintptr_t>(user->buf), 32u));
}

void __cdecl Hunk_UserDestroy(HunkUser* user) {
    FreeHunkChain(user);
}

char* __cdecl Hunk_CopyString(HunkUser* user, const char* in) {
    if (!user || !in)
        return nullptr;
    const std::size_t length = std::strlen(in) + 1u;
    auto* copy = static_cast<char*>(Hunk_UserAlloc(user, static_cast<uint32_t>(length), 1));
    if (copy)
        std::memcpy(copy, in, length);
    return copy;
}

uint8_t* __cdecl Hunk_Alloc(uint32_t size, const char*, int) {
    auto* allocation = static_cast<uint8_t*>(VitaHunkAllocInternal(size, 32));
    if (g_hunkTraceCount++ < 64u) {
        std::printf("[kisak-vita] source hunk alloc size=%u ptr=%p used=%u/%u\n",
                    size, static_cast<void*>(allocation),
                    static_cast<unsigned>(g_hunkPos),
                    static_cast<unsigned>(kVitaHunkCapacity));
    }
    return allocation;
}

uint8_t* __cdecl Hunk_AllocAlign(uint32_t size, int alignment, const char*, int) {
    return static_cast<uint8_t*>(VitaHunkAllocInternal(size, alignment));
}

uint8_t* __cdecl Hunk_AllocLow(uint32_t size, const char* name, int type) {
    return Hunk_Alloc(size, name, type);
}

uint8_t* __cdecl Hunk_AllocLowAlign(uint32_t size, int alignment, const char* name, int type) {
    return Hunk_AllocAlign(size, alignment, name, type);
}

uint8_t* __cdecl Hunk_AllocXAnimPrecache(uint32_t size) {
    return Hunk_Alloc(size, "Hunk_AllocXAnimPrecache", 11);
}

uint8_t* __cdecl Hunk_AllocPhysPresetPrecache(uint32_t size) {
    return Hunk_Alloc(size, "Hunk_AllocPhysPresetPrecache", 21);
}

uint8_t* __cdecl Hunk_AllocXModelPrecache(uint32_t size) {
    return Hunk_Alloc(size, "Hunk_AllocXModelPrecache", 21);
}

uint8_t* __cdecl Hunk_AllocXModelPrecacheColl(uint32_t size) {
    return Hunk_Alloc(size, "Hunk_AllocXModelPrecacheColl", 27);
}

uint8_t* __cdecl Hunk_AllocXAnimServer(uint32_t size) {
    uint8_t* allocation = Hunk_Alloc(size, "Hunk_AllocXAnimServer", 11);
    char detail[128]{};
    std::snprintf(detail, sizeof(detail),
                  "size=%u ptr=%p used=%u capacity=%u base=%p",
                  size, static_cast<void*>(allocation),
                  static_cast<unsigned>(g_hunkPos),
                  static_cast<unsigned>(kVitaHunkCapacity),
                  static_cast<void*>(s_hunkData));
    kisak::vita::VitaScriptServicesTrace("source_hunk_xanim", nullptr,
                                         detail);
    std::printf("[kisak-vita] source hunk XAnim %s\n", detail);
    return allocation;
}

uint32_t* __cdecl Hunk_AllocateTempMemory(int size, const char* name) {
    return static_cast<uint32_t*>(AllocateSourceTemp(size, name));
}

void __cdecl Hunk_FreeTempMemory(char* buf) {
    if (!buf)
        return;
    if (g_tempHigh.Untrack(buf) || !Hunk_DataOnHunk(reinterpret_cast<uint8_t*>(buf)))
        std::free(buf);
}

void Hunk_ClearTempMemory() { Hunk_ClearTempMemoryHigh(); }
void Hunk_CheckTempMemoryClear() {}

void Hunk_ClearToMarkLow(int mark) {
    if (mark >= 0 && g_sourceHunk.Rewind(static_cast<size_t>(mark))) {
        g_hunkPos = static_cast<std::size_t>(mark);
        s_hunkData = s_origHunkData = g_sourceHunk.Base();
        s_hunkTotal = static_cast<int>(g_sourceHunk.Reserved());
    }
}

int Hunk_SetMarkLow() { return static_cast<int>(g_hunkPos); }
int Hunk_Used() { return static_cast<int>(g_hunkPos); }

bool __cdecl Hunk_DataOnHunk(uint8_t* data) {
    return g_sourceHunk.Contains(data);
}

void* __cdecl Hunk_FindDataForFile(int type, const char* name) {
    auto* record = FindVitaFileData(type, name ? name : "");
    return record ? record->data : nullptr;
}

void* __cdecl Hunk_FindDataForFileInternal(int type, const char* name, int) {
    return Hunk_FindDataForFile(type, name);
}

char* __cdecl Hunk_SetDataForFile(int type, const char* name, void* data, void* (__cdecl*)(int)) {
    const char* safeName = name ? name : "";
    if (auto* existing = FindVitaFileData(type, safeName)) {
        existing->data = data;
        return existing->name;
    }
    auto* record = NewVitaFileData();
    if (!record)
        return nullptr;
    record->type = type;
    record->data = data;
    std::strncpy(record->name, safeName, sizeof(record->name) - 1u);
    record->name[sizeof(record->name) - 1u] = '\0';
    return record->name;
}

void __cdecl Hunk_AddData(int type, void* data, void* (__cdecl*)(int)) {
    if (auto* record = NewVitaFileData()) {
        record->type = type;
        record->data = data;
    }
}

void __cdecl Hunk_OverrideDataForFile(int type, const char* name, void* data) {
    if (auto* record = FindVitaFileData(type, name ? name : "")) {
        record->data = data;
        return;
    }
    (void)Hunk_SetDataForFile(type, name, data, nullptr);
}

void Hunk_ClearData() {
    for (auto& record : g_fileData)
        record = {};
}

void __cdecl Hunk_ClearDataFor(fileData_s**, uint8_t*, uint8_t*) { Hunk_ClearData(); }

void* __cdecl Z_VirtualReserve(int size) {
    return size > 0 ? static_cast<uint8_t*>(std::calloc(1, static_cast<std::size_t>(size))) : nullptr;
}

void __cdecl Z_VirtualDecommitInternal(void*, int) {}
void __cdecl Z_VirtualFreeInternal(void* ptr) { std::free(ptr); }
void* __cdecl Z_TryVirtualAllocInternal(int size) { return Z_VirtualReserve(size); }
bool __cdecl Z_TryVirtualCommitInternal(void*, int) { return true; }
void __cdecl Z_VirtualCommitInternal(void*, int) {}
void __cdecl Z_VirtualDecommit(void*, int) {}
void __cdecl Z_VirtualCommit(void*, int) {}

char* __cdecl Z_VirtualAlloc(int size, const char*, int) {
    return size > 0 ? static_cast<char*>(std::calloc(1, static_cast<std::size_t>(size))) : nullptr;
}

char* __cdecl Z_MallocGarbage(int size, const char*, int) {
    return Z_VirtualAlloc(size, "", 0);
}

void* Z_Malloc(int size, const char*, int) { return Z_VirtualAlloc(size, "", 0); }
void Z_Free(void* ptr, int) { std::free(ptr); }

uint32_t __cdecl LargeLocalRoundSize(int size) {
    return size > 0 ? static_cast<uint32_t>((size + 3) & ~3) : 0u;
}

int __cdecl LargeLocalBegin(int size) {
    if (!g_largeLocalBuf)
        g_largeLocalBuf = static_cast<unsigned char*>(std::calloc(1, kLargeLocalCapacity));
    const std::size_t rounded = LargeLocalRoundSize(size);
    if (!g_largeLocalBuf || rounded > kLargeLocalCapacity - g_largeLocalPos)
        return -1;
    const int start = static_cast<int>(g_largeLocalPos);
    g_largeLocalPos += rounded;
    return start;
}

void __cdecl LargeLocalEnd(int startPos) {
    if (startPos >= 0 && static_cast<std::size_t>(startPos) <= g_largeLocalPos)
        g_largeLocalPos = static_cast<std::size_t>(startPos);
}

uint8_t* __cdecl LargeLocalGetBuf(int startPos) {
    if (startPos < 0 || !g_largeLocalBuf || static_cast<std::size_t>(startPos) >= kLargeLocalCapacity)
        return nullptr;
    return &g_largeLocalBuf[startPos];
}

void __cdecl LargeLocalReset() { g_largeLocalPos = 0; }

LargeLocal::LargeLocal(int sizeParam) : startPos(LargeLocalBegin(sizeParam)), size(sizeParam) {}
LargeLocal::~LargeLocal() { LargeLocalEnd(startPos); }
uint8_t* LargeLocal::GetBuf() { return LargeLocalGetBuf(startPos); }

void* Hunk_AllocDebugMem(uint32_t size) {
    if (size == 0)
        size = 1;
    void* allocation = std::calloc(1, size);
    if (g_debugHunkTraceCount++ < 64u) {
        std::printf("[kisak-vita] source debug alloc size=%u ptr=%p\n",
                    size, allocation);
    }
    return allocation;
}

void __cdecl Hunk_FreeDebugMem(void* ptr) {
    std::free(ptr);
}

char* __cdecl TempMalloc(uint32_t len) {
    return g_tempUser ? static_cast<char*>(Hunk_UserAlloc(g_tempUser, len, 1)) : nullptr;
}

char* __cdecl TempMallocAlignStrict(uint32_t len) {
    return g_tempUser ? static_cast<char*>(Hunk_UserAllocAlignStrict(g_tempUser, len)) : nullptr;
}

void __cdecl TempMemoryReset(HunkUser* user) { g_tempUser = user; }

void __cdecl TempMemorySetPos(char* pos) {
    if (g_tempUser && pos)
        Hunk_UserSetPos(g_tempUser, reinterpret_cast<uint8_t*>(pos));
}

uint32_t __cdecl Hunk_AllocateTempMemoryHigh(int size, const char* name) {
    void* memory = AllocateSourceTemp(size, name);
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(memory));
}

void Hunk_ClearTempMemoryHigh() {
    g_tempHigh.Clear();
}

void Hunk_CheckTempMemoryHighClear() {}

char* __cdecl Z_TryVirtualAlloc(int size, const char*, int) {
    if (size <= 0)
        return nullptr;
    return static_cast<char*>(std::calloc(1, static_cast<std::size_t>(size)));
}

void __cdecl Z_VirtualFree(void* ptr) { std::free(ptr); }

KISAK_VITA_WEAK void __cdecl Com_Memcpy(void* dest, const void* src, uint32_t count) {
    if (dest && src && count)
        std::memcpy(dest, src, count);
}

__attribute__((weak)) int __cdecl Com_sprintf(char* dest, uint32_t size, const char* fmt, ...) {
    if (!dest || size == 0 || !fmt)
        return -1;
    va_list args;
    va_start(args, fmt);
    const int result = std::vsnprintf(dest, size, fmt, args);
    va_end(args);
    dest[size - 1] = '\0';
    return result;
}

__attribute__((weak)) bool I_iscsym(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

__attribute__((weak)) void I_strncpyz(char* dest, const char* src, int destsize) {
    if (!dest || destsize <= 0)
        return;
    std::strncpy(dest, src ? src : "", static_cast<std::size_t>(destsize - 1));
    dest[destsize - 1] = '\0';
}

__attribute__((weak)) int I_stricmp(const char* left, const char* right) {
    const unsigned char* a = reinterpret_cast<const unsigned char*>(left ? left : "");
    const unsigned char* b = reinterpret_cast<const unsigned char*>(right ? right : "");
    while (*a && *b) {
        const int ca = (*a >= 'A' && *a <= 'Z') ? *a + ('a' - 'A') : *a;
        const int cb = (*b >= 'A' && *b <= 'Z') ? *b + ('a' - 'A') : *b;
        if (ca != cb)
            return ca - cb;
        ++a;
        ++b;
    }
    return static_cast<int>(*a) - static_cast<int>(*b);
}

__attribute__((weak)) const char* I_stristr(const char* text, const char* needle) {
    if (!text || !needle || !needle[0])
        return text;
    for (const char* cursor = text; *cursor; ++cursor) {
        const char* a = cursor;
        const char* b = needle;
        while (*a && *b) {
            const unsigned char ca = static_cast<unsigned char>(*a);
            const unsigned char cb = static_cast<unsigned char>(*b);
            const unsigned char la = (ca >= 'A' && ca <= 'Z') ? static_cast<unsigned char>(ca + ('a' - 'A')) : ca;
            const unsigned char lb = (cb >= 'A' && cb <= 'Z') ? static_cast<unsigned char>(cb + ('a' - 'A')) : cb;
            if (la != lb)
                break;
            ++a;
            ++b;
        }
        if (!*b)
            return cursor;
    }
    return nullptr;
}

void Com_PrintMessage(int, const char* message, int) {
    if (message) {
        kisak::vita::VitaScriptServicesTrace("source_console", message);
        sceClibPrintf("[kisak-source] %s", message);
    }
}

void Com_PrintError(int, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    RecordError(fmt, args);
    va_end(args);
}

void __cdecl Com_Error(errorParm_t, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    RecordError(fmt, args);
    va_end(args);
    if (g_errorBoundary)
        ::longjmp(g_errorBoundary->jump, 1);
}

void __cdecl Sys_Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    RecordError(fmt, args);
    va_end(args);
    if (g_errorBoundary)
        ::longjmp(g_errorBoundary->jump, 1);
}

int __cdecl Sys_IsRemoteDebugClient() { return 0; }

void __cdecl Scr_InitDebuggerMain() {}
void __cdecl Scr_InitDebugger() {}
void __cdecl Scr_InitDebuggerSystem() {}
void __cdecl Scr_ShutdownDebuggerSystem(int) {}
void __cdecl Scr_AddAssignmentPos(char*) {}

void __cdecl ProfLoad_Begin(const char*) {}
void __cdecl ProfLoad_End() {}

ProfileScript profileScript = {};
int __cdecl Profile_AddScriptName(char* profileName) {
    if (!profileName)
        return -1;
    for (int index = 0; index < 40; ++index) {
        if (profileScript.profileScriptNames[index][0] == '\0') {
            I_strncpyz(profileScript.profileScriptNames[index], profileName, 20);
            return index;
        }
        if (I_stricmp(profileScript.profileScriptNames[index], profileName) == 0)
            return index;
    }
    return -1;
}

KISAK_VITA_WEAK int32_t __cdecl CL_GetLocalClientActiveCount() { return 1; }

const dvar_s* fs_gameDirVar = &g_gameDir;
// Dvar values and registration are owned by universal/dvar.cpp.

XAssetHeader __cdecl DB_FindXAssetHeader(XAssetType type, const char* name) {
    XAssetHeader result{};
    if (type == ASSET_TYPE_RAWFILE) {
        if (MountedRawFile* file = FindMountedRawFile(name))
            result.rawfile = &file->raw;
    }
    if (!result.data)
        result.data = kisak::vita::VitaSourcePlatform_FindAsset(
            static_cast<uint32_t>(type), name);
    return result;
}

uint32_t __cdecl FS_FOpenFileRead(const char* name, int* file) {
    if (!file)
        return 0xFFFFFFFFu;
    *file = 0;
    MountedRawFile* record = FindMountedRawFile(name);
    if (!record)
        return 0xFFFFFFFFu;
    for (OpenRawFile& open : g_openRawFiles) {
        if (open.used)
            continue;
        open.used = true;
        open.handle = 16 + static_cast<int>(&open - g_openRawFiles);
        open.record = static_cast<std::size_t>(record - g_rawFiles);
        open.offset = 0;
        *file = open.handle;
        return static_cast<uint32_t>(record->length);
    }
    return 0xFFFFFFFFu;
}

uint32_t __cdecl FS_FOpenFileByMode(char* name, int* file, fsMode_t mode) {
    if (mode != FS_READ)
        return 0xFFFFFFFFu;
    return FS_FOpenFileRead(name, file);
}

uint32_t __cdecl FS_Read(uint8_t* buffer, uint32_t len, int file) {
    OpenRawFile* open = FindOpenRawFile(file);
    if (!buffer || !open)
        return 0;
    if (open->stream)
        return static_cast<uint32_t>(std::fread(buffer, 1, len, open->stream));
    if (open->record >= g_rawFileCount) return 0;
    const MountedRawFile& record = g_rawFiles[open->record];
    if (!record.bytes || open->offset >= record.length)
        return 0;
    const std::size_t available = record.length - open->offset;
    const uint32_t count = static_cast<uint32_t>(std::min<std::size_t>(len, available));
    std::memcpy(buffer, record.bytes + open->offset, count);
    open->offset += count;
    return count;
}

void __cdecl FS_FCloseFile(int file) {
    if (OpenRawFile* open = FindOpenRawFile(file)) {
        if (open->stream) std::fclose(open->stream);
        *open = {};
    }
}

bool FS_Initialized() { return g_rawFileBatch; }
bool FS_IsFileInZip(int) { return false; } // raw records and loose stream files
uint32_t FS_FOpenFileReadStream(const char* name, int* file) {
    const uint32_t mounted = FS_FOpenFileRead(name, file);
    if (mounted != UINT32_MAX || !file) return mounted;
    // Only the locally generated stream tree is exposed to source filesystem
    // reads. Source paths cannot select Vita partitions or escape that tree.
    if (!name || !name[0] || name[0] == '/' || name[0] == '\\'
        || std::strchr(name, ':') || std::strstr(name, "..")) return UINT32_MAX;
    char path[256];
    const int length = std::snprintf(path, sizeof(path), "ux0:data/CODVIT001/stream/%s", name);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) return UINT32_MAX;
    // COD4 authored stream paths use both separators and mixed case.
    for (char* at = path + std::strlen("ux0:data/CODVIT001/stream/"); *at; ++at) {
        if (*at == '\\') *at = '/';
        else if (*at >= 'A' && *at <= 'Z') *at = static_cast<char>(*at + ('a' - 'A'));
    }
    for (OpenRawFile& open : g_openRawFiles) {
        if (open.used) continue;
        FILE* stream = std::fopen(path, "rb");
        if (!stream) return UINT32_MAX;
        if (std::fseek(stream, 0, SEEK_END) != 0) { std::fclose(stream); return UINT32_MAX; }
        const long bytes = std::ftell(stream);
        if (bytes < 0 || std::fseek(stream, 0, SEEK_SET) != 0) { std::fclose(stream); return UINT32_MAX; }
        open = {}; open.used = true; open.stream = stream;
        open.handle = 16 + static_cast<int>(&open - g_openRawFiles);
        *file = open.handle;
        return static_cast<uint32_t>(bytes);
    }
    return UINT32_MAX;
}
uint32_t FS_FTell(int file) {
    OpenRawFile* open = FindOpenRawFile(file);
    if (!open) return UINT32_MAX;
    return open->stream ? static_cast<uint32_t>(std::ftell(open->stream)) : static_cast<uint32_t>(open->offset);
}
int FS_Seek(int file, int offset, int origin) {
    OpenRawFile* open = FindOpenRawFile(file);
    if (!open || (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)) return -1;
    if (open->stream) return std::fseek(open->stream, offset, origin);
    const int64_t length = g_rawFiles[open->record].length;
    const int64_t base = origin == SEEK_END ? length : origin == SEEK_CUR ? open->offset : 0;
    const int64_t position = base + offset;
    if (position < 0 || position > length) return -1;
    open->offset = static_cast<size_t>(position);
    return 0;
}

int FS_GetFileList(const char* path, const char* extension, FsListBehavior_e,
                    char* listbuf, int bufsize) {
    if (!listbuf || bufsize <= 0 || !path || !extension) return 0;
    listbuf[0] = '\0';
    size_t pathLength = std::strlen(path);
    while (pathLength && path[pathLength - 1] == '/') --pathLength;
    const size_t extensionLength = std::strlen(extension);
    const char* matches[kMaxMountedRawFiles];
    size_t count = 0;
    for (size_t i = 0; i < g_rawFileCount; ++i) {
        const char* name = g_rawFiles[i].name;
        if (pathLength) {
            if (std::strlen(name) <= pathLength || I_strnicmp(name, path, pathLength) || name[pathLength] != '/') continue;
            name += pathLength + 1;
        }
        const size_t length = std::strlen(name);
        if (std::strchr(name, '/') || length < extensionLength
            || I_stricmp(name + length - extensionLength, extension)) continue;
        matches[count++] = name;
    }
    std::sort(matches, matches + count, [](const char* a, const char* b) { return I_stricmp(a, b) < 0; });
    size_t used = 0;
    int written = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t bytes = std::strlen(matches[i]) + 1;
        if (bytes > static_cast<size_t>(bufsize) - used) break;
        std::memcpy(listbuf + used, matches[i], bytes);
        used += bytes; ++written;
    }
    return written;
}

int __cdecl FS_ReadFile(const char* qpath, void** buffer) {
    MountedRawFile* record = FindMountedRawFile(qpath);
    if (!record || !RawFileBytes(record))
        return -1;
    if (!buffer)
        return static_cast<int>(record->length);
    auto* copy = static_cast<char*>(std::malloc(record->length + 1u));
    if (!copy)
        return -1;
    std::memcpy(copy, record->bytes, record->length);
    copy[record->length] = '\0';
    *buffer = copy;
    return static_cast<int>(record->length);
}

void __cdecl FS_FreeFile(char* buffer) { std::free(buffer); }
void __cdecl FS_FreeMem(char* buffer) { std::free(buffer); }

int __cdecl FS_FOpenFileWrite(const char*) { return 2; }
int __cdecl FS_FOpenTextFileWrite(const char*) { return 2; }
int __cdecl FS_OpenFileOverwrite(char*) { return 2; }
uint32_t __cdecl FS_Write(const char*, uint32_t len, int handle) {
    return handle == 2 ? len : 0u;
}
void FS_Printf(int handle, const char* fmt, ...) {
    if (handle != 2 || !fmt)
        return;
    va_list args;
    va_start(args, fmt);
    va_end(args);
}
void __cdecl FS_FreeFileList(const char** list) { std::free(const_cast<char**>(list)); }
const char** __cdecl FS_ListFiles(const char*, const char*, FsListBehavior_e, int* numfiles) {
    if (numfiles)
        *numfiles = 0;
    return nullptr;
}

char* __cdecl Com_LoadRawTextFile(const char* fullpath) {
    void* buffer = nullptr;
    return FS_ReadFile(fullpath, &buffer) >= 0 ? static_cast<char*>(buffer) : nullptr;
}

void __cdecl Com_UnloadRawTextFile(char* filebuf) { FS_FreeFile(filebuf); }

char* __cdecl Com_LoadInfoString(char* fileName, const char* fileDesc,
                                 const char* ident, char* loadBuffer) {
    // KVAP assets replace desktop fastfiles, so retain the fastfile text
    // contract here: the rawfile begins with an identifying token and the
    // caller parses only the bytes following that token.  Returning the
    // header made GRAPH_FLOAT_FILE look like a zero-knot graph.
    (void)loadBuffer;
    if (!fileName || !fileDesc || !ident) {
        Com_Error(ERR_DROP, "Invalid info-string load request");
        return nullptr;
    }

    RawFile* rawfile = DB_FindXAssetHeader(ASSET_TYPE_RAWFILE, fileName).rawfile;
    if (!rawfile || !rawfile->buffer) {
        Com_Error(ERR_DROP, "Could not load %s file [%s]", fileDesc, fileName);
        return nullptr;
    }

    const std::size_t identLength = std::strlen(ident);
    if (rawfile->len < 0 || static_cast<std::size_t>(rawfile->len) < identLength
        || std::memcmp(rawfile->buffer, ident, identLength) != 0) {
        Com_Error(ERR_DROP, "File [%s] is not a %s file", fileName, fileDesc);
        return nullptr;
    }

    char* contents = const_cast<char*>(rawfile->buffer + identLength);
    if (!Info_Validate(contents)) {
        Com_Error(ERR_DROP, "File [%s] is not a valid %s", fileName, fileDesc);
        return nullptr;
    }
    return contents;
}

void __cdecl Com_SyncThreads() {}

KISAK_VITA_WEAK char __cdecl Com_Filter(const char* filter, char* name, int casesensitive) {
    if (!filter || !name)
        return false;
    const auto equal = [casesensitive](char left, char right) {
        if (casesensitive)
            return left == right;
        if (left >= 'A' && left <= 'Z') left = static_cast<char>(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z') right = static_cast<char>(right + ('a' - 'A'));
        return left == right;
    };
    const char* star = nullptr;
    const char* mark = nullptr;
    const char* cursor = filter;
    char* value = name;
    while (*value) {
        if (*cursor == '*') {
            star = ++cursor;
            mark = value;
            continue;
        }
        if (*cursor == '?' || equal(*cursor, *value)) {
            ++cursor;
            ++value;
            continue;
        }
        if (star) {
            cursor = star;
            value = ++mark;
            continue;
        }
        return false;
    }
    while (*cursor == '*')
        ++cursor;
    return *cursor == '\0';
}

struct ComWorld comWorld = {};
KISAK_VITA_WEAK const ComPrimaryLight* __cdecl Com_GetPrimaryLight(uint32_t index) {
    return comWorld.primaryLights && index < comWorld.primaryLightCount
        ? &comWorld.primaryLights[index] : nullptr;
}

static dvar_t g_vitaFsBasepath = {};
static dvar_t g_vitaLocWarnings = {};
static dvar_t g_vitaLocWarningsAsErrors = {};
static dvar_t g_vitaClShownet = {};
static dvar_t g_vitaClShownuments = {};
static dvar_t g_vitaClProfileTextY = {};
static dvar_t g_vitaClProfileTextHeight = {};
static dvar_t g_vitaCgPacketAnalysisClient = {};
static dvar_t g_vitaRModelVertColor = {};

const dvar_t* fs_basepath = &g_vitaFsBasepath;
KISAK_VITA_WEAK const dvar_t* loc_warnings = &g_vitaLocWarnings;
KISAK_VITA_WEAK const dvar_t* loc_warningsAsErrors = &g_vitaLocWarningsAsErrors;
KISAK_VITA_WEAK const dvar_t* cl_shownet = &g_vitaClShownet;
KISAK_VITA_WEAK const dvar_t* cl_shownuments = &g_vitaClShownuments;
KISAK_VITA_WEAK const dvar_t* cl_profileTextY = &g_vitaClProfileTextY;
KISAK_VITA_WEAK const dvar_t* cl_profileTextHeight = &g_vitaClProfileTextHeight;
KISAK_VITA_WEAK const dvar_t* cg_packetAnalysisClient = &g_vitaCgPacketAnalysisClient;
const dvar_t* r_modelVertColor = &g_vitaRModelVertColor;
int com_fixedConsolePosition = 0;
int com_numConsoleLines = 0;
char* com_consoleLines[32] = {};

long double msecPerRawTimerTick = 1.0L;
void __cdecl track_shutdown(int) {}
void __cdecl BG_ShutdownWeaponDefFiles();
#include <qcommon/com_weapon_memory.inc>

void kisak::vita::VitaScriptServicesReleaseWeaponInfo() {
    // Partial startup may fail before SV_Shutdown can reach G_ShutdownGame.
    // Release the actual source owner while dvars/script strings are still live.
    const int previousOwner = weaponInfoSource;
    if (weaponInfoSource != WEAPMEMSOURCE_NONE)
        Com_FreeWeaponInfoMemory(weaponInfoSource);
    char detail[64]{};
    std::snprintf(detail, sizeof(detail), "owner_before=%d owner_after=%d",
                  previousOwner, weaponInfoSource);
    VitaScriptServicesTrace("weapon_memory_release", "before source teardown", detail);
}


double __cdecl R_GetAdjustedLodDist(float dist, XModelLodRampType) { return dist; }
double __cdecl R_GetBaseLodDist(const float* origin) {
    if (!origin)
        return 0.0;
    return std::sqrt(static_cast<double>(origin[0]) * origin[0]
                   + static_cast<double>(origin[1]) * origin[1]
                   + static_cast<double>(origin[2]) * origin[2]);
}

void __cdecl R_GetXModelBounds(XModel*, const float (*)[3], float* mins, float* maxs) {
    if (!mins || !maxs)
        return;
    mins[0] = mins[1] = mins[2] = 0.0f;
    maxs[0] = maxs[1] = maxs[2] = 0.0f;
}

PhysGeomList* __cdecl XModel_LoadPhysicsCollMap(const char*, void* (__cdecl*)(int)) { return nullptr; }

void __cdecl Profile_BeginScript(int) {}
void __cdecl Profile_BeginScripts(uint32_t) {}
void __cdecl Profile_EndScript(int) {}
void __cdecl Profile_EndScripts(uint32_t) {}

KISAK_VITA_WEAK clientActive_t clients[STATIC_MAX_LOCAL_CLIENTS] = {};
KISAK_VITA_WEAK clientConnection_t clientConnections[STATIC_MAX_LOCAL_CLIENTS] = {};

KISAK_VITA_WEAK clientConnection_t *__cdecl CL_GetLocalClientConnection(int32_t localClientNum) {
    if (localClientNum < 0 || localClientNum >= STATIC_MAX_LOCAL_CLIENTS)
        return &clientConnections[0];
    return &clientConnections[localClientNum];
}

KISAK_VITA_WEAK char __cdecl CL_AnyLocalClientsRunning() { return 0; }
KISAK_VITA_WEAK uint8_t __cdecl CG_ShouldPlaySoundOnLocalClient() { return 0; }
KISAK_VITA_WEAK void __cdecl CL_ForwardCommandToServer(int, const char*) {}
__attribute__((weak)) void __cdecl CL_Netchan_AddOOBProfilePacket(int, int) {}
__attribute__((weak)) void __cdecl CL_Netchan_PrintProfileStats(int, int) {}
__attribute__((weak)) void __cdecl CL_Netchan_UpdateProfileStats(int) {}
__attribute__((weak)) void __cdecl CL_FlushDebugServerData() {}
__attribute__((weak)) void __cdecl CL_UpdateDebugServerData() {}
KISAK_VITA_WEAK void __cdecl CL_DrawString(int, int, char*, int, int) {}
KISAK_VITA_WEAK void __cdecl CL_DrawRect(int, int, int, int, const float*) {}
void __cdecl CL_AddDebugLine(const float*, const float*, const float*, int, int, int) {}
void __cdecl CL_AddDebugString(const float*, const float*, float, const char*, int, int) {}
void __cdecl CL_AddDebugStar(const float*, const float*, int, int) {}
void __cdecl CL_AddDebugStarWithText(const float*, const float*, const float*, const char*, float, int, int) {}
KISAK_VITA_WEAK float (*__cdecl CL_GetMapCenter())[3] {
    static float center[3] = {};
    return &center;
}
KISAK_VITA_WEAK bool __cdecl CL_GetPredictedOriginForServerTime(clientActive_t*, int, float*, float*, float*, int*, int*) {
    return false;
}

void __cdecl G_DebugLine(const float*, const float*, const float*, int) {}
void __cdecl G_DebugLineWithDuration(const float*, const float*, const float*, int, int) {}
void __cdecl G_DebugStar(const float*, const float*) {}
void __cdecl G_DebugStarWithText(const float*, const float*, const float*, char*, float) {}
void __cdecl G_DebugBox(const float*, const float*, const float*, float, const float*, int, int) {}
void __cdecl G_DebugCircle(const float*, float, const float*, int, int, int) {}
void __cdecl G_DebugCircleEx(const float*, float, const float*, const float*, int, int) {}

bool __cdecl DB_IsMinimumFastFileLoaded() { return false; }
bool __cdecl DB_IsXAssetDefault(XAssetType type, const char* name) {
    // A validated, arena-owned registry object is a real fastfile-equivalent
    // asset. Reporting every lookup as the default made
    // BG_GetWeaponIndexForName discard correctly rehydrated VITP records and
    // silently collapse the fixed loadout to WP_NONE.
    return kisak::vita::VitaSourcePlatform_FindAsset(
               static_cast<uint32_t>(type), name) == nullptr;
}
int32_t __cdecl DB_GetAllXAssetOfType_FastFile(XAssetType, XAssetHeader*, int32_t) { return 0; }
void __cdecl DB_LoadXAssets(XZoneInfo*, uint32_t, int32_t) {}
void __cdecl DB_SyncXAssets() {}
void __cdecl DB_ResetZoneSize(int32_t) {}
void __cdecl DB_ReplaceModel(const char*, const char*) {}
const char* __cdecl DB_GetXAssetTypeName(uint32_t) { return "unknown"; }

char* __cdecl SV_GetMapBaseName(char* mapName) {
    if (!mapName)
        return nullptr;
    char* slash = std::strrchr(mapName, '/');
    return slash ? slash + 1 : mapName;
}
void __cdecl SV_Heartbeat_f() {}
KISAK_VITA_WEAK const char* __cdecl SEH_StringEd_GetString(const char* reference) { return reference ? reference : ""; }

// The retail debugger's UI/remote socket is not part of the Vita slice yet,
// but the VM still calls these hooks on normal notify/error paths.  Keep the
// callbacks weak and side-effect free so a future Vita debugger can replace
// them without changing the VM ABI.
__attribute__((weak)) void __cdecl Scr_CheckBreakonNotify(uint32_t, uint32_t, VariableValue*, char*, uint32_t) {}
__attribute__((weak)) void __cdecl Scr_DebugKillThread(uint32_t, const char*) {}
__attribute__((weak)) void __cdecl Scr_DebugTerminateThread(int) {}
__attribute__((weak)) int __cdecl Scr_HitAssignmentBreakpoint(VariableValue*, char*, uint32_t, int) { return 0; }
__attribute__((weak)) int __cdecl Scr_HitBreakpoint(VariableValue*, char*, uint32_t, int) { return 0; }
__attribute__((weak)) void __cdecl Scr_HitBuiltinBreakpoint(VariableValue*, const char*, uint32_t, int, int, uint32_t) {}
__attribute__((weak)) bool __cdecl Scr_IgnoreErrors() { return false; }
__attribute__((weak)) void __cdecl Scr_ShowConsole() {}
__attribute__((weak)) void __cdecl Scr_ShutdownDebugger() {}
__attribute__((weak)) void __cdecl Scr_ShutdownDebuggerMain() {}
__attribute__((weak)) void __cdecl Scr_UpdateDebugger() {}
__attribute__((weak)) void __cdecl Scr_MonitorCommand(const char*) {}

XModel* __cdecl R_RegisterModel(const char* name) {
    return static_cast<XModel*>(
        kisak::vita::VitaSourcePlatform_FindAsset(ASSET_TYPE_XMODEL, name));
}
Material* __cdecl Material_RegisterHandle(const char* name, int) {
    return static_cast<Material*>(
        kisak::vita::VitaSourcePlatform_FindAsset(ASSET_TYPE_MATERIAL, name));
}
KISAK_VITA_WEAK const FxEffectDef* __cdecl FX_Register(const char* name) {
    return static_cast<const FxEffectDef*>(
        kisak::vita::VitaSourcePlatform_FindAsset(ASSET_TYPE_FX, name));
}

// Source asset parsers use the engine string table for pointer stability.
// Preserve that ownership contract at the Vita platform boundary instead of
// handing EffectsCore a pointer into a temporary KVAP staging buffer.
void __cdecl ReplaceString(const char** destination, const char* value) {
    if (!destination || !value)
        return;
    const char* replacement = SL_ConvertToString(
        SL_GetString_(value, 0, MT_TYPE_GENERIC));
    if (*destination) {
        const uint32_t oldString = SL_FindString(*destination);
        if (oldString)
            SL_RemoveRefToString(oldString);
    }
    *destination = replacement;
}

float __cdecl CosOfSumOfArcCos(float left, float right) {
    const float clampedLeft = std::max(-1.0f, std::min(1.0f, left));
    const float clampedRight = std::max(-1.0f, std::min(1.0f, right));
    return std::cos(std::acos(clampedLeft) + std::acos(clampedRight));
}

bool __cdecl Com_LogFileOpen() {
#if KISAK_VITA_CPU_RUNTIME
    return kisak::vita::VitaCpuProfile_Read().active;
#else
    return g_traceFile != nullptr;
#endif
}
