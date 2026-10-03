#include <cstddef>
#include <cstdint>

#include <server_mp/server_mp.h>

static_assert(alignof(serverStatic_t) >= 128);
static_assert(sizeof(entityState_s) == 0xF4);
static_assert(sizeof(clientState_s) == 0x64);
static_assert(sizeof(cachedSnapshot_t) == 0x1C);
static_assert(sizeof(archivedSnapshot_s) == 0x8);
#if defined(KISAK_VITA_SLICE)
// Milestone 1's one-client capacity profile must fit before any live server
// lifecycle work is credited.  The Vita-specific ring/client macros in
// server_mp.h are the source of truth; enforce the budget in the ARM object
// probe so a future capacity change cannot silently regress the footprint.
static_assert(sizeof(serverStatic_t) < 8u * 1024u * 1024u,
              "KISAK Vita serverStatic_t exceeds the 8 MiB slice budget");
#endif

extern "C" uint32_t kisak_vita_server_layout_probe() {
    return static_cast<uint32_t>(sizeof(serverStatic_t) ^
                                 (sizeof(client_t) << 1) ^
                                 (sizeof(cachedClient_s) << 2));
}
