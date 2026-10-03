#include <client_mp/client_mp.h>
#include <server_mp/server_mp.h>

// This executable is a linker worklist, not a playable fallback. Taking the
// addresses forces section-GC to retain the actual server/client frame roots
// and therefore exposes every still-missing platform or source subsystem at
// link time without running an uninitialized global engine on the device.
int main() {
    volatile auto serverFrame = &SV_Frame;
    volatile auto clientFrame = &CL_Frame;
    return serverFrame && clientFrame ? 0 : 2;
}
