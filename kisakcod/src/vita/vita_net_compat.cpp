#include "vita_net_compat.h"

#include <psp2/kernel/threadmgr.h>
#include <cstring>

namespace {

constexpr uint32_t kSnapshotClients = 8;
constexpr uint32_t kSnapshotDepth = 4;
// Source MP snapshots are Huffman-compressed payloads, not the former
// 100-byte DMS1 mirror. Keep enough room for a one-client playerstate plus
// future entity/client deltas without turning the loopback into an unbounded
// allocator on Vita.
constexpr uint32_t kSnapshotBytes = 4096;

struct SnapshotPacket {
    uint32_t length = 0;
    uint8_t bytes[kSnapshotBytes]{};
};

struct SnapshotQueue {
    uint32_t write = 0;
    uint32_t read = 0;
    SnapshotPacket packets[kSnapshotDepth]{};
};

SnapshotQueue g_snapshotQueues[kSnapshotClients];
SnapshotQueue g_matchStateQueues[kSnapshotClients];
SnapshotQueue g_sourceSnapshotQueues[kSnapshotClients];
SnapshotQueue g_commandQueues[kSnapshotClients];

bool SendPacket(SnapshotQueue* queues, uint32_t clientNum, const void* data, uint32_t length)
{
    if (!queues || clientNum >= kSnapshotClients || !data || length == 0 || length > kSnapshotBytes)
        return false;
    SnapshotQueue& queue = queues[clientNum];
    if (queue.write - queue.read >= kSnapshotDepth)
        ++queue.read;
    SnapshotPacket& packet = queue.packets[queue.write % kSnapshotDepth];
    packet.length = length;
    std::memcpy(packet.bytes, data, length);
    ++queue.write;
    return true;
}

bool ReceivePacket(SnapshotQueue* queues, uint32_t clientNum, void* data, uint32_t capacity,
                   uint32_t* length)
{
    if (length)
        *length = 0;
    if (!queues || clientNum >= kSnapshotClients || !data || !length)
        return false;
    SnapshotQueue& queue = queues[clientNum];
    if (queue.read >= queue.write)
        return false;
    const SnapshotPacket& packet = queue.packets[queue.read % kSnapshotDepth];
    if (packet.length > capacity)
        return false;
    std::memcpy(data, packet.bytes, packet.length);
    *length = packet.length;
    ++queue.read;
    return true;
}

} // namespace

qboolean Sys_GetPacket(netadr_t *net_from, msg_t *net_message)
{
    if (net_from)
        std::memset(net_from, 0, sizeof(*net_from));
    if (net_message)
        net_message->cursize = 0;
    // External UDP is deliberately disabled.  Local sessions use the exact
    // engine loopback ring in net_chan_mp.cpp instead of this socket path.
    return qfalse;
}

qboolean Sys_GetBroadcastPacket(msg_t *net_message)
{
    if (net_message)
        net_message->cursize = 0;
    return qfalse;
}

char __cdecl Sys_SendPacket(int, unsigned __int8 *, netadr_t)
{
    // No Internet or LAN socket is opened by the Vita slice.
    return 0;
}

qboolean Sys_StringToAdr(const char *s, netadr_t *a)
{
    if (!a)
        return qfalse;
    std::memset(a, 0, sizeof(*a));
    if (!s || !std::strcmp(s, "localhost") || !std::strcmp(s, "loopback")
        || !std::strcmp(s, "127.0.0.1"))
    {
        a->type = NA_LOOPBACK;
        return qtrue;
    }
    a->type = NA_BAD;
    return qfalse;
}

bool __cdecl Sys_IsLANAddress(netadr_t adr)
{
    return adr.type == NA_LOOPBACK || adr.type == NA_BOT;
}

bool __cdecl Sys_IsLANAddress_IgnoreSubnet(netadr_t adr)
{
    return Sys_IsLANAddress(adr);
}

void NET_Sleep(int msec)
{
    if (msec > 0)
        sceKernelDelayThread(static_cast<unsigned int>(msec) * 1000u);
}

bool VitaLoopback_SendSnapshot(uint32_t clientNum, const void* data, uint32_t length)
{
    return SendPacket(g_snapshotQueues, clientNum, data, length);
}

bool VitaLoopback_ReceiveSnapshot(uint32_t clientNum, void* data, uint32_t capacity,
                                  uint32_t* length)
{
    return ReceivePacket(g_snapshotQueues, clientNum, data, capacity, length);
}

bool VitaLoopback_SendMatchState(uint32_t clientNum, const void* data, uint32_t length)
{
    return SendPacket(g_matchStateQueues, clientNum, data, length);
}

bool VitaLoopback_ReceiveMatchState(uint32_t clientNum, void* data, uint32_t capacity,
                                    uint32_t* length)
{
    return ReceivePacket(g_matchStateQueues, clientNum, data, capacity, length);
}

bool VitaLoopback_SendSourceSnapshot(uint32_t clientNum, const void* data, uint32_t length)
{
    return SendPacket(g_sourceSnapshotQueues, clientNum, data, length);
}

bool VitaLoopback_ReceiveSourceSnapshot(uint32_t clientNum, void* data, uint32_t capacity,
                                        uint32_t* length)
{
    return ReceivePacket(g_sourceSnapshotQueues, clientNum, data, capacity, length);
}

bool VitaLoopback_SendCommand(uint32_t clientNum, const void* data, uint32_t length)
{
    return SendPacket(g_commandQueues, clientNum, data, length);
}

bool VitaLoopback_ReceiveCommand(uint32_t clientNum, void* data, uint32_t capacity,
                                 uint32_t* length)
{
    return ReceivePacket(g_commandQueues, clientNum, data, capacity, length);
}

void VitaLoopback_Reset()
{
    for (SnapshotQueue& queue : g_snapshotQueues)
        queue = SnapshotQueue{};
    for (SnapshotQueue& queue : g_matchStateQueues)
        queue = SnapshotQueue{};
    for (SnapshotQueue& queue : g_sourceSnapshotQueues)
        queue = SnapshotQueue{};
    for (SnapshotQueue& queue : g_commandQueues)
        queue = SnapshotQueue{};
}
