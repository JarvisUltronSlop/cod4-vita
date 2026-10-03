#include <qcommon/qcommon.h>

// Shared unchanged with the desktop filesystem; Vita's package mount must
// resolve the same multiplayer asset identity as the original fastfile path.
void __cdecl Com_GetBspFilename(char* filename, uint32_t size, const char* mapname)
{
#ifdef KISAK_MP
    Com_sprintf(filename, size, "maps/mp/%s.d3dbsp", mapname);
#elif KISAK_SP
    Com_sprintf(filename, size, "maps/%s.d3dbsp", mapname);
#endif
}
