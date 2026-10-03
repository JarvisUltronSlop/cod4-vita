#include <vita/vita_hunk_arena.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifndef _MSC_VER
#define __cdecl
#endif

static void Check(bool ok) { if (!ok) std::abort(); }
static void MyAssertHandler(const char*, int, int, const char*, ...) {
    throw std::logic_error("source weapon ownership assertion");
}
#define iassert(condition) do { if (!(condition)) MyAssertHandler(__FILE__, __LINE__, 0, ""); } while (0)

static kisak::vita::VitaHunkArena arena;
static unsigned parsedWeaponIndex = 0;
static unsigned shutdownCalls = 0;
static const char* animType = nullptr;

// Isolate the BG boundary. Its reset invalidates G_SetupWeaponDef's load gate;
// the callback must still have live hunk storage, just as in G_ShutdownGame.
static void __cdecl BG_ShutdownWeaponDefFiles() {
    Check(arena.Contains(animType));
    Check(std::strcmp(animType, "briefcase") == 0);
    parsedWeaponIndex = 0;
    ++shutdownCalls;
}

// Execute the same source implementation compiled by desktop Com and Vita.
#include <qcommon/com_weapon_memory.inc>

static void LoadWeaponInfo(int owner) {
    if (!parsedWeaponIndex) {
        Com_SetWeaponInfoMemory(owner);
        auto* storage = static_cast<char*>(arena.Allocate(10, 4));
        Check(storage != nullptr);
        std::memcpy(storage, "briefcase", 10);
        animType = storage;
        parsedWeaponIndex = 5;
    }
    Check(arena.Contains(animType));
    Check(std::strcmp(animType, "briefcase") == 0);
}

int main() {
    for (unsigned cycle = 0; cycle < 10; ++cycle) {
        // Server-owned local matches and client-owned partial startup both
        // release only through the matching owner, before the map hunk resets.
        const int owner = cycle % 2 ? 2 : 1;
        const int other = owner == 1 ? 2 : 1;
        LoadWeaponInfo(owner);
        Com_FreeWeaponInfoMemory(other);
        Check(weaponInfoSource == owner && parsedWeaponIndex == 5);
        Check(shutdownCalls == cycle);
        LoadWeaponInfo(owner);
        Com_FreeWeaponInfoMemory(owner);
        Check(weaponInfoSource == 0 && parsedWeaponIndex == 0);
        Check(shutdownCalls == cycle + 1);
        Com_FreeWeaponInfoMemory(owner);
        Check(shutdownCalls == cycle + 1);
        Check(arena.Rewind(0) && arena.Reserved() == 0);
        animType = nullptr;
    }
    bool rejected = false;
    try { Com_SetWeaponInfoMemory(3); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected && weaponInfoSource == 0);
    LoadWeaponInfo(1);
    rejected = false;
    try { Com_SetWeaponInfoMemory(2); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected && weaponInfoSource == 1);
    Com_FreeWeaponInfoMemory(1);
    arena.Clear();
    std::puts("source weapon ownership: ten reloads, non-owner release, partial-start owner and duplicate release passed");
}
