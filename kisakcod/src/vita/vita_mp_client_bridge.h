#pragma once

#include <cstdint>

#include <qcommon/msg_mp.h>

struct playerState_s;

namespace kisak::vita {

struct ShipmentDmPlayerState;

// Mirrors the authoritative one-client local-DM result into the shared MP
// client state.  The Vita transport remains bounded and local, but renderer,
// HUD, prediction, and future source client code can consume the same
// clientActive_t snapshot/command ring as the desktop path.
void VitaMpClientBridge_Initialize();
void VitaMpClientBridge_InitializeParserState();
void VitaMpClientBridge_SubmitCommand(const usercmd_s& command);
void VitaMpClientBridge_PredictCommand(const usercmd_s& command);
void VitaMpClientBridge_ApplySnapshot(const ShipmentDmPlayerState& snapshot,
                                      int32_t serverTime);
// Copy the last authoritative playerstate installed by the real
// CL_ParseSnapshot path.  The caller owns the destination so the bridge can
// keep its clientActive_t storage private while the renderer consumes the
// same parsed pose as the desktop cgame would.
bool VitaMpClientBridge_CopyParsedPlayerState(::playerState_s* output);
// Stage the authoritative Shipment result for the native server writer while
// preserving the client's previous parsed frame for source delta handling.
void VitaMpClientBridge_StageSnapshot(const ShipmentDmPlayerState& snapshot,
                                      int32_t serverTime);

// Put the compressed source snapshot on the Vita-local server/client ring.
// The parser consumes it through VitaMpClientBridge_ParseSnapshotMessage.
bool VitaMpClientBridge_QueueSnapshotMessage(int32_t serverTime);

// Decode the compressed source snapshot currently mirrored in clients[0]
// through KisakCOD's real CL_ParseSnapshot implementation. This is kept
// separate from ApplySnapshot so the Vita local server can remain the
// authoritative simulation while the client state is validated through the
// source network format.
bool VitaMpClientBridge_ParseSnapshotMessage(int32_t serverTime);

// Build a real MP svc_snapshot/playerstate delta message from the mirrored
// state. The result is the Huffman-compressed payload consumed by
// CL_ParseServerMessage after the desktop netchan/reliable header. The caller
// owns the buffer; zero means the message could not fit or the source writer
// reported overflow. Entity/client sections are emitted as empty terminators
// for the one-client Shipment slice.
uint32_t VitaMpClientBridge_BuildSnapshotMessage(uint8_t* buffer,
                                                 uint32_t capacity,
                                                 int32_t serverTime);

} // namespace kisak::vita
