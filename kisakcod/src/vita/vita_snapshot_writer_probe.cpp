#include <cstdint>

#include <bgame/bg_local.h>
#include <qcommon/sv_msg_write_mp.h>

int main() {
    uint8_t storage[16384] = {};
    msg_t message{};
    MSG_Init(&message, storage, static_cast<int>(sizeof(storage)));

    SnapshotInfo_s info{};
    info.clientNum = 0;
    info.snapshotDeltaTime = 0;
    // Archived mode keeps this focused probe independent of a live server
    // client header while exercising the real MP playerstate delta writer.
    info.archived = true;
    info.fromBaseline = true;

    playerState_s state{};
    state.commandTime = 1000;
    state.clientNum = 0;
    state.origin[0] = 1.0f;
    state.origin[1] = 2.0f;
    state.origin[2] = 3.0f;
    state.stats[0] = 100;
    state.weapon = 1;

    MSG_WriteByte(&message, 6); // svc_snapshot
    MSG_WriteLong(&message, state.commandTime);
    MSG_WriteByte(&message, 0); // no delta frame
    MSG_WriteByte(&message, 0); // snapshot flags
    MSG_WriteDeltaPlayerstate(&info, &message, state.commandTime, nullptr, &state);
    MSG_WriteByte(&message, 7); // svc_EOF
    return message.overflowed ? 2 : 0;
}
