#include <universal/q_shared.h>
#include <win32/win_local.h>
#if defined(KISAK_VITA_SLICE)
#include <vita/vita_net_compat.h>
#else
#include <win32/win_net.h>
#endif

void Sys_LockWrite(FastCriticalSection* critSect)
{
    while (1)
    {
        if (critSect->readCount == 0)
        {
            if (InterlockedIncrement(&critSect->writeCount) == 1 && critSect->readCount == 0)
            {
                break;
            }
            InterlockedDecrement(&critSect->writeCount);
        }
        NET_Sleep(0);
    }
}

void Sys_UnlockWrite(FastCriticalSection* critSect)
{
    iassert(critSect->writeCount > 0);
    InterlockedDecrement(&critSect->writeCount);
}

