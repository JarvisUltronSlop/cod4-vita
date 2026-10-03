#pragma once

#include <qcommon/net_chan_mp.h>

// Platform declarations for the loopback-first Vita net shim.  External UDP
// remains a disabled feature, but the shared netchan code can still compile
// and use the exact engine call shapes.
qboolean Sys_GetPacket(netadr_t *net_from, msg_t *net_message);
qboolean Sys_GetBroadcastPacket(msg_t *net_message);
char __cdecl Sys_SendPacket(int length, unsigned __int8 *data, netadr_t to);
qboolean Sys_StringToAdr(const char *s, netadr_t *a);
bool __cdecl Sys_IsLANAddress(netadr_t adr);
bool __cdecl Sys_IsLANAddress_IgnoreSubnet(netadr_t adr);
void NET_Sleep(int msec);

// Fixed-capacity local command/snapshot transport used by the offline DM
// slice. It is deliberately separate from external UDP; each packet ABI is a
// bounded little-endian byte envelope and can later be replaced by the shared
// netchan without changing the client/server payload shape.
bool VitaLoopback_SendSnapshot(uint32_t clientNum, const void* data, uint32_t length);
bool VitaLoopback_ReceiveSnapshot(uint32_t clientNum, void* data, uint32_t capacity,
                                  uint32_t* length);
// Match metadata is carried on its own bounded lane so the existing
// player-snapshot ABI remains unchanged.  The lane is local-only, just like
// the player snapshot ring, and is consumed by the Vita DM client before HUD
// presentation.
bool VitaLoopback_SendMatchState(uint32_t clientNum, const void* data, uint32_t length);
bool VitaLoopback_ReceiveMatchState(uint32_t clientNum, void* data, uint32_t capacity,
                                    uint32_t* length);
bool VitaLoopback_SendSourceSnapshot(uint32_t clientNum, const void* data, uint32_t length);
bool VitaLoopback_ReceiveSourceSnapshot(uint32_t clientNum, void* data, uint32_t capacity,
                                        uint32_t* length);
bool VitaLoopback_SendCommand(uint32_t clientNum, const void* data, uint32_t length);
bool VitaLoopback_ReceiveCommand(uint32_t clientNum, void* data, uint32_t capacity,
                                 uint32_t* length);
void VitaLoopback_Reset();
void VitaSourceLoopback_Reset();
