// Original KisakCOD weapon-info lifetime, shared by desktop Com and Vita.
// Include once per platform after qcommon declarations and BG_ShutdownWeaponDefFiles.
// Kept separate so the Vita platform boundary cannot replace these with no-ops.
#ifndef WEAPMEMSOURCE_NONE
#define WEAPMEMSOURCE_NONE 0
#endif
int weaponInfoSource = WEAPMEMSOURCE_NONE;

void __cdecl Com_SetWeaponInfoMemory(int source)
{
    if (source != 1 && source != 2)
        MyAssertHandler(
            ".\\qcommon\\common.cpp",
            4551,
            0,
            "%s",
            "(source == WEAPMEMSOURCE_SERVER) || (source == WEAPMEMSOURCE_CLIENT)");
    iassert( weaponInfoSource == WEAPMEMSOURCE_NONE );
    weaponInfoSource = source;
}

void __cdecl Com_FreeWeaponInfoMemory(int source)
{
    if (source != 1 && source != 2)
        MyAssertHandler(
            ".\\qcommon\\common.cpp",
            4560,
            0,
            "%s",
            "(source == WEAPMEMSOURCE_SERVER) || (source == WEAPMEMSOURCE_CLIENT)");
    if (source == weaponInfoSource)
    {
        weaponInfoSource = 0;
        BG_ShutdownWeaponDefFiles();
    }
}

