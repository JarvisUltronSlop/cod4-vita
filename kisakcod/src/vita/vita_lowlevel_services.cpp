#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vita/vita_cpu_profile.h>
#include <vita/vita_cpu_replay.h>

#include <psp2/kernel/threadmgr/lw_mutex.h>
#include <psp2/kernel/threadmgr/thread.h>

#ifndef __cdecl
#define __cdecl
#endif

#include <universal/q_shared.h>
#include <universal/com_math.h>
#include <qcommon/threads.h>
#include <qcommon/qcommon.h>

int Sys_InterlockedIncrement(uint32_t* addend) {
    return static_cast<int>(InterlockedIncrement(addend));
}
int Sys_InterlockedDecrement(uint32_t* addend) {
    return static_cast<int>(InterlockedDecrement(addend));
}

namespace {

constexpr std::size_t kInfoCapacity = 1024;
thread_local char g_infoValue[2][8192] = {};
thread_local unsigned int g_infoValueIndex = 0;
thread_local char g_vtos[64] = {};
thread_local void* g_sysValues[4] = {};
thread_local bool g_threadContextInitialized = false;

// The MP layout currently has 22 named critical sections.  Keep a little
// headroom here so this platform boundary does not silently become unsafe if
// another source subsystem is added to the Vita link before the enum is
// extended in a shared header.
constexpr int kVitaCriticalSectionCount = 32;
alignas(8) SceKernelLwMutexWork
    g_criticalSections[kVitaCriticalSectionCount]{};
// 0 = uninitialized, 1 = one thread is creating it, 2 = ready.
std::atomic<unsigned int>
    g_criticalSectionState[kVitaCriticalSectionCount]{};

bool EnsureCriticalSection(int index) {
    if (index < 0 || index >= kVitaCriticalSectionCount)
        return false;

    unsigned int state = g_criticalSectionState[index].load(
        std::memory_order_acquire);
    if (state == 2u)
        return true;

    unsigned int expected = 0u;
    if (g_criticalSectionState[index].compare_exchange_strong(
            expected, 1u, std::memory_order_acq_rel)) {
        char name[24]{};
        std::snprintf(name, sizeof(name), "kisak-crit-%02d", index);
        const int result = sceKernelCreateLwMutex(
            &g_criticalSections[index], name,
            SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, nullptr);
        g_criticalSectionState[index].store(result >= 0 ? 2u : 0u,
                                            std::memory_order_release);
        return result >= 0;
    }

    // Creation is a one-time operation measured in microseconds.  Yield
    // instead of burning a Vita core if an I/O/audio worker reaches the same
    // source lock during startup.
    while ((state = g_criticalSectionState[index].load(
                std::memory_order_acquire)) == 1u) {
        sceKernelDelayThread(50);
    }
    return state == 2u;
}

// The desktop thread bootstrap calls Com_InitThreadData before any engine
// service can use va(), profiling, or the per-thread error jump buffer.  Vita
// entry points are intentionally tiny and do not create the desktop worker
// thread graph, so lazily initialize the main-thread context at the common
// Sys_GetValue boundary.  This keeps standalone probes and the retail
// bootstrap on the same ABI contract without requiring every entry point to
// duplicate desktop startup plumbing.
void EnsureThreadContext() {
    if (g_threadContextInitialized)
        return;
    g_threadContextInitialized = true;
#if KISAK_VITA_CPU_RUNTIME
    const auto role = kisak::vita::VitaCpuCurrentRole();
    if (role == kisak::vita::VitaCpuRole::Source)
        Com_InitThreadData(THREAD_CONTEXT_MAIN);
    else if (role == kisak::vita::VitaCpuRole::Render)
        Com_InitThreadData(THREAD_CONTEXT_BACKEND);
    else {
        // Workers use private decoding/recording services, never engine TLS,
        // error boundaries, va() storage, or mutable source globals.
        kisak::vita::VitaCpuProfile_Counter("forbidden-worker-source-tls", uint32_t(role));
        std::abort();
    }
#else
    Com_InitThreadData(0);
#endif
}

bool InfoKeyEquals(const char* left, const char* right) {
    if (!left || !right)
        return false;
    while (*left && *right) {
        const unsigned char a = static_cast<unsigned char>(*left++);
        const unsigned char b = static_cast<unsigned char>(*right++);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return *left == *right;
}

bool InfoUnsafe(const char* value) {
    return value && (std::strchr(value, '\\') || std::strchr(value, ';') ||
                     std::strchr(value, '"'));
}

} // namespace

void Sys_InitializeCriticalSections() {
    for (int index = 0; index < kVitaCriticalSectionCount; ++index)
        (void)EnsureCriticalSection(index);
}

void Sys_EnterCriticalSection(int criticalSection) {
    if (!EnsureCriticalSection(criticalSection))
        return;
    (void)sceKernelLockLwMutex(&g_criticalSections[criticalSection], 1,
                              nullptr);
}

void Sys_LeaveCriticalSection(int criticalSection) {
    if (criticalSection < 0 || criticalSection >= kVitaCriticalSectionCount
        || g_criticalSectionState[criticalSection].load(
               std::memory_order_acquire) != 2u) {
        return;
    }
    (void)sceKernelUnlockLwMutex(&g_criticalSections[criticalSection], 1);
}

short __cdecl BigShort(short value) {
    const auto bits = static_cast<std::uint16_t>(value);
    return static_cast<short>(static_cast<std::uint16_t>((bits << 8) | (bits >> 8)));
}

__attribute__((weak)) void __cdecl Com_Memset(void* destination, int value, unsigned int count) {
    if (destination && count)
        std::memset(destination, value, count);
}

__attribute__((weak)) void __cdecl AnglesToAxis(const float* angles, float axis[3][3]) {
    if (!angles || !axis)
        return;
    constexpr float kDegrees = 3.14159265358979323846f / 180.0f;
    const float yaw = angles[1] * kDegrees;
    const float pitch = angles[0] * kDegrees;
    const float roll = angles[2] * kDegrees;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    axis[0][0] = cp * cy;
    axis[0][1] = cp * sy;
    axis[0][2] = -sp;
    axis[1][0] = sr * sp * cy - sy * cr;
    axis[1][1] = sr * sp * sy + cr * cy;
    axis[1][2] = sr * cp;
    axis[2][0] = cr * sp * cy + sr * sy;
    axis[2][1] = cr * sp * sy - sr * cy;
    axis[2][2] = cr * cp;
}

__attribute__((weak)) char* __cdecl I_strlwr(char* value) {
    if (!value)
        return value;
    for (char* cursor = value; *cursor; ++cursor)
        *cursor = static_cast<char>(std::tolower(static_cast<unsigned char>(*cursor)));
    return value;
}

__attribute__((weak)) void __cdecl I_strncat(char* destination, int capacity, const char* source) {
    if (!destination || !source || capacity <= 0)
        return;
    const std::size_t used = std::strlen(destination);
    if (used >= static_cast<std::size_t>(capacity - 1))
        return;
    std::strncat(destination, source, static_cast<std::size_t>(capacity - 1) - used);
    destination[capacity - 1] = '\0';
}

__attribute__((weak)) int __cdecl I_strncmp(const char* left, const char* right, int count) {
    if (!left || !right || count <= 0)
        return 0;
    for (int index = 0; index < count; ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        if (a != b)
            return a < b ? -1 : 1;
        if (a == '\0')
            return 0;
    }
    return 0;
}

__attribute__((weak)) std::uint8_t __cdecl I_CleanChar(std::uint8_t character) {
    return character == 146u ? static_cast<std::uint8_t>('\'') : character;
}

__attribute__((weak)) char* __cdecl I_CleanStr(char* value) {
    if (!value)
        return value;
    char* output = value;
    for (char* cursor = value; *cursor;) {
        if (cursor[0] == '^' && cursor[1] && cursor[1] != '^' &&
            cursor[1] >= '0' && cursor[1] <= '9') {
            cursor += 2;
            continue;
        }
        const unsigned char character = static_cast<unsigned char>(*cursor++);
        if (character >= 32u && character != 127u)
            *output++ = static_cast<char>(character);
    }
    *output = '\0';
    return value;
}

__attribute__((weak)) bool __cdecl Info_Validate(const char* value) { return value && !InfoUnsafe(value); }

__attribute__((weak)) const char* __cdecl Info_ValueForKey(const char* info, const char* key) {
    char* result = g_infoValue[g_infoValueIndex++ & 1u];
    result[0] = '\0';
    if (!info || !key)
        return result;
    const char* cursor = info;
    if (*cursor == '\\')
        ++cursor;
    while (*cursor) {
        char parsedKey[256] = {};
        char parsedValue[8192] = {};
        std::size_t keyLength = 0;
        while (*cursor && *cursor != '\\' && keyLength + 1 < sizeof(parsedKey))
            parsedKey[keyLength++] = *cursor++;
        parsedKey[keyLength] = '\0';
        if (*cursor == '\\')
            ++cursor;
        std::size_t valueLength = 0;
        while (*cursor && *cursor != '\\' && valueLength + 1 < sizeof(parsedValue))
            parsedValue[valueLength++] = *cursor++;
        parsedValue[valueLength] = '\0';
        if (InfoKeyEquals(parsedKey, key)) {
            std::strncpy(result, parsedValue, 8191);
            result[8191] = '\0';
            return result;
        }
        if (*cursor == '\\')
            ++cursor;
    }
    return result;
}

__attribute__((weak)) void __cdecl Info_SetValueForKey(char* info, const char* key, const char* value) {
    if (!info || !key || !value || !key[0] || InfoUnsafe(key) || InfoUnsafe(value))
        return;
    char rebuilt[kInfoCapacity] = {};
    const char* cursor = info;
    if (*cursor == '\\')
        ++cursor;
    bool replaced = false;
    while (*cursor) {
        char parsedKey[256] = {};
        char parsedValue[768] = {};
        std::size_t keyLength = 0;
        while (*cursor && *cursor != '\\' && keyLength + 1 < sizeof(parsedKey))
            parsedKey[keyLength++] = *cursor++;
        parsedKey[keyLength] = '\0';
        if (*cursor == '\\')
            ++cursor;
        std::size_t valueLength = 0;
        while (*cursor && *cursor != '\\' && valueLength + 1 < sizeof(parsedValue))
            parsedValue[valueLength++] = *cursor++;
        parsedValue[valueLength] = '\0';
        if (!InfoKeyEquals(parsedKey, key)) {
            I_strncat(rebuilt, static_cast<int>(sizeof(rebuilt)), "\\");
            I_strncat(rebuilt, static_cast<int>(sizeof(rebuilt)), parsedKey);
            I_strncat(rebuilt, static_cast<int>(sizeof(rebuilt)), "\\");
            I_strncat(rebuilt, static_cast<int>(sizeof(rebuilt)), parsedValue);
        } else {
            replaced = true;
        }
        if (*cursor == '\\')
            ++cursor;
    }
    I_strncat(rebuilt, static_cast<int>(sizeof(rebuilt)), "\\");
    I_strncat(rebuilt, static_cast<int>(sizeof(rebuilt)), key);
    I_strncat(rebuilt, static_cast<int>(sizeof(rebuilt)), "\\");
    I_strncat(rebuilt, static_cast<int>(sizeof(rebuilt)), value);
    if (std::strlen(rebuilt) < kInfoCapacity)
        std::strcpy(info, rebuilt);
    (void)replaced;
}

__attribute__((weak)) const char* __cdecl vtos(const float* vector) {
    if (!vector)
        return "(0 0 0)";
    std::snprintf(g_vtos, sizeof(g_vtos), "(%g %g %g)",
                  static_cast<double>(vector[0]), static_cast<double>(vector[1]),
                  static_cast<double>(vector[2]));
    return g_vtos;
}

std::uint32_t __cdecl Sys_Milliseconds() {
    static const auto start = std::chrono::steady_clock::now();
#if KISAK_VITA_CPU_PROFILE
    uint32_t replayTime=0;
    if(kisak::vita::VitaCpuReplay_Clock(&replayTime))return replayTime;
#endif
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count());
}

std::uint32_t __cdecl Sys_MillisecondsRaw() { return Sys_Milliseconds(); }
bool __cdecl Sys_IsMainThread() {
#if KISAK_VITA_CPU_RUNTIME
    return kisak::vita::VitaCpuCurrentRole() == kisak::vita::VitaCpuRole::Source;
#else
    return true;
#endif
}
bool __cdecl Sys_IsRenderThread() {
#if KISAK_VITA_CPU_RUNTIME
    return kisak::vita::VitaCpuCurrentRole() == kisak::vita::VitaCpuRole::Render;
#else
    return false;
#endif
}
bool __cdecl Sys_IsDatabaseThread() { return false; }

void __cdecl Sys_SetValue(int valueIndex, void* data) {
    if (valueIndex >= 0 && valueIndex < static_cast<int>(sizeof(g_sysValues) / sizeof(g_sysValues[0])))
        g_sysValues[valueIndex] = data;
}

void* __cdecl Sys_GetValue(int valueIndex) {
    EnsureThreadContext();
    return valueIndex >= 0 && valueIndex < static_cast<int>(sizeof(g_sysValues) / sizeof(g_sysValues[0]))
        ? g_sysValues[valueIndex] : nullptr;
}

WinThreadLock __cdecl Win_GetThreadLock() { return THREAD_LOCK_NONE; }

// Portable form of the engine's collision broad-phase plane test.  The
// original desktop build dispatches through an x86 jump table keyed by
// signbits; the arithmetic below preserves the same front/back bit contract
// without relying on architecture-specific assembly.
__attribute__((weak)) int __cdecl BoxOnPlaneSide(const float* emins,
                                                  const float* emaxs,
                                                  const cplane_s* plane) {
    if (!emins || !emaxs || !plane || plane->signbits >= 8)
        return 3;

    float front;
    float back;
    switch (plane->signbits) {
    case 0:
        front = plane->normal[0] * emaxs[0] + plane->normal[1] * emaxs[1] + plane->normal[2] * emaxs[2];
        back = plane->normal[0] * emins[0] + plane->normal[1] * emins[1] + plane->normal[2] * emins[2];
        break;
    case 1:
        front = plane->normal[0] * emins[0] + plane->normal[1] * emaxs[1] + plane->normal[2] * emaxs[2];
        back = plane->normal[0] * emaxs[0] + plane->normal[1] * emins[1] + plane->normal[2] * emins[2];
        break;
    case 2:
        front = plane->normal[0] * emaxs[0] + plane->normal[1] * emins[1] + plane->normal[2] * emaxs[2];
        back = plane->normal[0] * emins[0] + plane->normal[1] * emaxs[1] + plane->normal[2] * emins[2];
        break;
    case 3:
        front = plane->normal[0] * emins[0] + plane->normal[1] * emins[1] + plane->normal[2] * emaxs[2];
        back = plane->normal[0] * emaxs[0] + plane->normal[1] * emaxs[1] + plane->normal[2] * emins[2];
        break;
    case 4:
        front = plane->normal[0] * emaxs[0] + plane->normal[1] * emaxs[1] + plane->normal[2] * emins[2];
        back = plane->normal[0] * emins[0] + plane->normal[1] * emins[1] + plane->normal[2] * emaxs[2];
        break;
    case 5:
        front = plane->normal[0] * emins[0] + plane->normal[1] * emaxs[1] + plane->normal[2] * emins[2];
        back = plane->normal[0] * emaxs[0] + plane->normal[1] * emins[1] + plane->normal[2] * emaxs[2];
        break;
    case 6:
        front = plane->normal[0] * emaxs[0] + plane->normal[1] * emins[1] + plane->normal[2] * emins[2];
        back = plane->normal[0] * emins[0] + plane->normal[1] * emaxs[1] + plane->normal[2] * emaxs[2];
        break;
    default:
        front = plane->normal[0] * emins[0] + plane->normal[1] * emins[1] + plane->normal[2] * emins[2];
        back = plane->normal[0] * emaxs[0] + plane->normal[1] * emaxs[1] + plane->normal[2] * emaxs[2];
        break;
    }
    return (2 * (back < plane->dist)) | (front > plane->dist);
}

void __cdecl Com_PrintWarning(int channel, const char* format, ...) {
    char message[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);
    Com_PrintMessage(channel, message, 0);
}
