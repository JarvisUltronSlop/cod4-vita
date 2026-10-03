#pragma once

#include <cstdint>

using BYTE = uint8_t;
using LONG = int32_t;

#ifndef ARRAYSIZE
#define ARRAYSIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef HIWORD
#define HIWORD(x) (static_cast<uint16_t>((static_cast<uint32_t>(x) >> 16) & 0xFFFFu))
#endif

// The script pools are single-threaded during the initial Vita bring-up, but
// retain the Win32 call shape so the shared implementation remains unchanged.
enum CriticalSection : int
{
    CRITSECT_SCRIPT_STRING = 0x6,
    CRITSECT_MEMORY_TREE = 0x7,
};

inline void Sys_EnterCriticalSection(int) {}
inline void Sys_LeaveCriticalSection(int) {}

template <typename T>
inline T InterlockedIncrement(volatile T *value)
{
    return __atomic_add_fetch(value, static_cast<T>(1), __ATOMIC_SEQ_CST);
}

template <typename T>
inline T InterlockedDecrement(volatile T *value)
{
    return __atomic_sub_fetch(value, static_cast<T>(1), __ATOMIC_SEQ_CST);
}

template <typename T, typename Exchange, typename Compare>
inline T InterlockedCompareExchange(volatile T *value, Exchange exchange, Compare comparand)
{
    T expected = static_cast<T>(comparand);
    const T desired = static_cast<T>(exchange);
    __atomic_compare_exchange_n(value, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
