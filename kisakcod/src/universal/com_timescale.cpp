#include <qcommon/qcommon.h>

// Shared source definitions extracted from qcommon/common.cpp so the Vita
// composition root can retain the original audio timing without desktop init.
const dvar_t* com_fixedtime;
const dvar_t* com_timescale;
const dvar_t* dev_timescale;

double __cdecl Com_GetTimescaleForSnd()
{
    if (com_fixedtime->current.integer)
        return (double)com_fixedtime->current.integer;
    return (float)(com_timescale->current.value * dev_timescale->current.value);
}
