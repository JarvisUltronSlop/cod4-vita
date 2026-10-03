#pragma once

// The Vita slice is intentionally offline and loopback-only.  These narrow
// stubs preserve the server's auth call shapes while making it impossible for
// the target to reach Steam or an Internet auth service.
inline bool Steam_CheckClientTicket(const void *, uint32_t, uint64_t)
{
    return true;
}
inline void Steam_OnClientDropped(uint64_t) {}
inline void Steam_CheckClients() {}
