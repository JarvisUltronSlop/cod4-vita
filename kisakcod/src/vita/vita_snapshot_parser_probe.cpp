#include <cstdint>

#include <client_mp/client_mp.h>
#include <qcommon/msg_mp.h>
#include <vita/vita_mp_client_bridge.h>
#include <vita/vita_shipment_dm.h>

int main() {
    kisak::vita::VitaMpClientBridge_Initialize();
    kisak::vita::VitaMpClientBridge_InitializeParserState();

    kisak::vita::ShipmentDmPlayerState state{};
    state.connected = true;
    state.alive = true;
    state.clientNum = 0;
    state.origin[0] = 32.0f;
    state.origin[1] = -16.0f;
    state.origin[2] = 16.0f;
    state.viewHeight = 60.0f;
    state.weaponSlot = 1;
    state.clipAmmo = 30;
    state.reserveAmmo = 90;
    state.health = 100;
    state.angles[1] = 90.0f;

    // Stage only the authoritative server state; the client snapshot must be
    // populated by the serialized source packet below.
    kisak::vita::VitaMpClientBridge_StageSnapshot(state, 1000);
    // Exercise the same native server-writer → Netchan → Vita ring → source
    // parser path used by the running bootstrap.
    if (!kisak::vita::VitaMpClientBridge_QueueSnapshotMessage(1000))
        return 2;
    if (!kisak::vita::VitaMpClientBridge_ParseSnapshotMessage(1000))
        return 3;
    return clients[0].snap.valid && clients[0].snap.serverTime == 1000 ? 0 : 4;
}
