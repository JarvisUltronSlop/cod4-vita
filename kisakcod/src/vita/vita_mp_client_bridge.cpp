#include <vita/vita_mp_client_bridge.h>

#include <algorithm>
#include <cstring>

#include <bgame/bg_local.h>
#include <client_mp/client_mp.h>
#include <qcommon/net_chan_mp.h>
#include <qcommon/sv_msg_write_mp.h>
#include <server_mp/server_mp.h>
#include <vita/vita_net_compat.h>
#include <vita/vita_shipment_dm.h>
#include <vita/vita_script_services.h>

extern loopback_t loopbacks[2];
extern bool fakelagInitialized;

namespace kisak::vita {
namespace {

// Keep these aliases tied to KisakCOD's declarations.  The old Vita bridge
// used bit 2 for prone, which is PMF_MANTLE in the source and made the HUD,
// camera, and animation state disagree during a stance transition.
constexpr int32_t kPmfCrouched = PMF_DUCKED;
constexpr int32_t kPmfProne = PMF_PRONE;
constexpr int32_t kPmfAds = PMF_SIGHT_AIMING;
constexpr int32_t kPmfSprinting = PMF_SPRINTING;
constexpr int32_t kPmfJumping = PMF_JUMPING;

// Keep one native server-side channel for the local one-client session.  The
// client-side channel lives in clientConnections[0], matching the source
// client layout.  The buffers are bounded to the source netchan limits and do
// not allocate per frame.
netchan_t g_sourceServerNetchan{};
char g_sourceServerOutgoing[131072]{};
char g_sourceServerIncoming[2048]{};
bool g_sourceNetchanInitialized = false;
playerState_s g_sourcePlayerState{};
bool g_sourcePlayerStateValid = false;
playerState_s g_predictedPlayerState{};
usercmd_s g_predictedOldCommand{};
bool g_predictionValid = false;

void Copy3(float* destination, const float* source) {
    std::memcpy(destination, source, sizeof(float) * 3);
}

int32_t WeaponIndex(uint32_t weaponSlot) {
    return std::clamp(static_cast<int32_t>(weaponSlot), 0, 127);
}

void BuildSourcePlayerState(const ShipmentDmPlayerState& snapshot,
                            int32_t serverTime,
                            playerState_s* player) {
    if (!player)
        return;
    *player = playerState_s{};
    const int32_t weapon = WeaponIndex(snapshot.weaponSlot);
    player->commandTime = serverTime;
    player->clientNum = static_cast<int32_t>(snapshot.clientNum);
    player->pm_type = snapshot.alive ? PM_NORMAL : PM_DEAD;
    player->otherFlags = POF_PLAYER;
    if (snapshot.crouched)
        player->pm_flags |= kPmfCrouched;
    if (snapshot.prone)
        player->pm_flags |= kPmfProne;
    if (snapshot.ads)
        player->pm_flags |= kPmfAds;
    if (snapshot.sprinting)
        player->pm_flags |= kPmfSprinting;
    if (snapshot.jumping)
        player->pm_flags |= kPmfJumping;
    Copy3(player->origin, snapshot.origin);
    Copy3(player->velocity, snapshot.velocity);
    Copy3(player->viewangles, snapshot.angles);
    player->viewHeightCurrent = snapshot.viewHeight;
    player->viewHeightTarget = static_cast<int32_t>(snapshot.viewHeight);
    player->fWeaponPosFrac = std::clamp(snapshot.weaponPosFrac, 0.0f, 1.0f);
    player->weapon = static_cast<uint32_t>(weapon);
    player->weaponstate = static_cast<weaponstate_t>(std::clamp(
        snapshot.weaponState, static_cast<int32_t>(WEAPON_READY),
        static_cast<int32_t>(WEAPON_NIGHTVISION_REMOVE)));
    player->offHandIndex = snapshot.grenadeCount > 0 ? 4 : 0;
    player->offhandSecondary = PLAYER_OFFHAND_SECONDARY_SMOKE;
    player->weapAnim = snapshot.weapAnim;
    player->ammoclip[weapon] = snapshot.clipAmmo;
    player->ammo[weapon] = snapshot.reserveAmmo;
    player->ammoclip[4] = std::clamp(snapshot.grenadeCount, 0, 2);
    player->ammo[4] = 0;
    player->weapons[0] = (1u << 1) | (1u << 2) | (1u << 3)
        | (snapshot.grenadeCount > 0 ? (1u << 4) : 0u);
    player->stats[0] = snapshot.health;
    player->groundEntityNum = ENTITYNUM_NONE;
    player->speed = 190;
    player->gravity = 800;
}

void PublishPredictedState() {
    if (!g_predictionValid)
        return;
    clientActive_t& client = clients[0];
    Copy3(client.cgameOrigin, g_predictedPlayerState.origin);
    Copy3(client.cgameVelocity, g_predictedPlayerState.velocity);
    Copy3(client.cgameViewangles, g_predictedPlayerState.viewangles);
    client.cgamePredictedDataServerTime = g_predictedPlayerState.commandTime;
}

netadr_t VitaLoopbackAddress(uint16_t port) {
    netadr_t address{};
    address.type = NA_LOOPBACK;
    address.port = port;
    return address;
}

void InitializeSourceNetchanDvars() {
    showpackets = Dvar_RegisterInt("showpackets", 0,
                                   (DvarLimits)0x200000000LL,
                                   DVAR_NOFLAG, "Show packets");
    showdrop = Dvar_RegisterBool("showdrop", false,
                                 DVAR_NOFLAG, "Show dropped packets");
    packetDebug = Dvar_RegisterBool("packetDebug", false,
                                    DVAR_NOFLAG, "Enable packet debugging information");
    net_profile = Dvar_RegisterInt("net_profile", 0,
                                   (DvarLimits)0x200000000LL,
                                   DVAR_NOFLAG, "Profile network performance");
    net_showprofile = Dvar_RegisterInt("net_showprofile", 0,
                                       (DvarLimits)0x300000000LL,
                                       DVAR_NOFLAG, "Show network profiling display");
    net_lanauthorize = Dvar_RegisterBool("net_lanauthorize", false,
                                         DVAR_NOFLAG, "Authorize LAN clients");
    msg_printEntityNums = Dvar_RegisterBool("msg_printEntityNums", false,
                                            DVAR_NOFLAG, "Print entity numbers");
    msg_dumpEnts = Dvar_RegisterBool("msg_dumpEnts", false,
                                     DVAR_NOFLAG, "Print snapshot entity info");
    msg_hudelemspew = Dvar_RegisterBool("msg_hudelemspew", false,
                                        DVAR_NOFLAG, "Debug HUD element fields");
    fakelag_current = Dvar_RegisterInt("fakelag_current", 0,
                                       (DvarLimits)0x3E700000000LL,
                                       DVAR_NOFLAG, "Current fake lag value");
    fakelag_target = Dvar_RegisterInt("fakelag_target", 0,
                                      (DvarLimits)0x3E700000000LL,
                                      DVAR_NOFLAG, "Target fake lag value");
    fakelag_currentjitter = Dvar_RegisterInt("fakelag_currentjitter", 0,
                                             (DvarLimits)0x3E700000000LL,
                                             DVAR_NOFLAG, "Current fake lag jitter");
    fakelag_jitter = Dvar_RegisterInt("fakelag_jitter", 0,
                                      (DvarLimits)0x3E700000000LL,
                                      DVAR_NOFLAG, "Fake lag jitter");
    fakelag_jitterinterval = Dvar_RegisterInt("fakelag_jitterinterval", 2000,
                                              (DvarLimits)0xEA6000000000LL,
                                              DVAR_NOFLAG, "Fake lag jitter interval");
    DvarLimits packetLossDomain{};
    packetLossDomain.value.min = 0.0;
    packetLossDomain.value.max = 1.0;
    fakelag_packetloss = Dvar_RegisterFloat("fakelag_packetloss", 0.0,
                                             packetLossDomain, DVAR_NOFLAG,
                                             "Fake lag packet loss");
    // Avoid the desktop FakeLag_Init command/dvar graph. Zero fake lag takes
    // the direct source loopback path and leaves the bounded packet queues
    // deterministic for the Vita slice.
    fakelagInitialized = false;
}

void PrepareNativeServerSnapshot(int32_t serverTime) {
    // The bounded Shipment server owns authoritative movement, while this
    // adapter projects its one-client state into the source MP snapshot
    // structures. This keeps the native writer fed by a real client_t/frame
    // rather than by a second hand-authored packet format.
    client_t& serverClient = svs.clients[0];
    serverClient.header.state = 4; // CS_ACTIVE
    serverClient.header.sendAsActive = 1;
    serverClient.header.deltaMessage = 0;
    serverClient.header.rateDelayed = 0;
    serverClient.header.netchan = g_sourceServerNetchan;
    std::memcpy(serverClient.name, "vita-local", 10);
    serverClient.name[10] = 0;

    const int frameIndex = g_sourceServerNetchan.outgoingSequence & PACKET_MASK;
    clientSnapshot_t& frame = serverClient.frames[frameIndex];
    frame.ps = g_sourcePlayerStateValid ? g_sourcePlayerState : clients[0].snap.ps;
    frame.serverTime = serverTime;
    frame.num_entities = 0;
    frame.num_clients = 0;
    frame.first_entity = 0;
    frame.first_client = 0;
    frame.messageSent = 0;
    frame.messageAcked = 0;
    frame.messageSize = 0;

    svs.time = serverTime;
    svs.snapFlagServerBit = 0;
    svs.numSnapshotEntities = 4096;
    svs.numSnapshotClients = 2048;
    svs.nextSnapshotEntities = 0;
    svs.nextSnapshotClients = 0;
    sv.num_entities = 0;
    sv.gentities = nullptr;
    // No entity records are emitted by this one-client source snapshot; the
    // native writer never dereferences the empty gentity table.
    sv.gentitySize = 0;

    std::memset(&svsHeader, 0, sizeof(svsHeader));
    svsHeader.clients = svs.clients;
    svsHeader.time = serverTime;
    svsHeader.snapFlagServerBit = svs.snapFlagServerBit;
    svsHeader.numSnapshotEntities = svs.numSnapshotEntities;
    svsHeader.numSnapshotClients = svs.numSnapshotClients;
    svsHeader.nextSnapshotEntities = svs.nextSnapshotEntities;
    svsHeader.nextSnapshotClients = svs.nextSnapshotClients;
    svsHeader.snapshotEntities = svs.snapshotEntities;
    svsHeader.snapshotClients = svs.snapshotClients;
    svsHeader.svEntities = sv.svEntities;
    svsHeader.cachedSnapshotEntities = svs.cachedSnapshotEntities;
    svsHeader.cachedSnapshotClients = svs.cachedSnapshotClients;
    svsHeader.archivedSnapshotBuffer = svs.archivedSnapshotBuffer;
    svsHeader.cachedSnapshotFrames = reinterpret_cast<cachedSnapshot_t*>(&svs);
    svsHeader.nextCachedSnapshotFrames = svs.nextCachedSnapshotFrames;
    svsHeader.nextArchivedSnapshotFrames = svs.nextArchivedSnapshotFrames;
    svsHeader.nextCachedSnapshotEntities = svs.nextCachedSnapshotEntities;
    svsHeader.nextCachedSnapshotClients = svs.nextCachedSnapshotClients;
    svsHeader.num_entities = sv.num_entities;
    svsHeader.maxclients = 1;
    svsHeader.fps = 30;
    svsHeader.clientArchive = 0;
    svsHeader.gentities = sv.gentities;
    svsHeader.gentitySize = sv.gentitySize;
    svsHeader.firstClientState = nullptr;
    svsHeader.firstPlayerState = nullptr;
    svsHeader.clientSize = sizeof(clientState_s);
    svsHeaderValid = 1;
}

} // namespace

void VitaMpClientBridge_Initialize() {
    clientActive_t& client = clients[0];
    std::memset(&client.snap, 0, sizeof(client.snap));
    g_sourcePlayerState = playerState_s{};
    g_sourcePlayerStateValid = false;
    g_predictedPlayerState = playerState_s{};
    g_predictedOldCommand = usercmd_s{};
    g_predictionValid = false;
    client.serverTime = 0;
    client.cmdNumber = 0;
    client.mouseIndex = 0;
    client.cgameFOVSensitivityScale = 1.0f;
    client.cgameMaxPitchSpeed = 140.0f;
    client.cgameMaxYawSpeed = 140.0f;
    client.viewangles[0] = 0.0f;
    client.viewangles[1] = 0.0f;
    client.viewangles[2] = 0.0f;

    std::memset(&clientConnections[0], 0, sizeof(clientConnections[0]));
    clientConnections[0].clientNum = 0;
    clientConnections[0].serverMessageSequence = 0;
    clientConnections[0].serverCommandSequence = 0;
    clientUIActives[0].active = 1;
    clientUIActives[0].isRunning = 1;
    clientUIActives[0].cgameInitialized = 0;
    clientUIActives[0].cgameInitCalled = 0;
    clientUIActives[0].connectionState = CA_CONNECTED;

    if (!g_sourceNetchanInitialized) {
        // Initialize only the source netchan dvars needed by
        // Netchan_Transmit/Netchan_Process. The desktop Netchan_Init command
        // graph is intentionally not entered by the bounded Vita bootstrap.
        InitializeSourceNetchanDvars();
        std::memset(loopbacks, 0, sizeof(loopbacks));
        const netadr_t loopback = VitaLoopbackAddress(0);
        Netchan_Setup(NS_SERVER, &g_sourceServerNetchan, loopback, 1,
                      g_sourceServerOutgoing, sizeof(g_sourceServerOutgoing),
                      g_sourceServerIncoming, sizeof(g_sourceServerIncoming));
        Netchan_Setup(NS_CLIENT1, &clientConnections[0].netchan, loopback, 1,
                      clientConnections[0].netchanOutgoingBuffer,
                      sizeof(clientConnections[0].netchanOutgoingBuffer),
                      clientConnections[0].netchanIncomingBuffer,
                      sizeof(clientConnections[0].netchanIncomingBuffer));
        g_sourceNetchanInitialized = true;
    }
}

void VitaMpClientBridge_InitializeParserState() {
    // The full desktop client registers these in CL_InitOnceForAllClients.
    // The bounded Vita bootstrap deliberately does not run that desktop
    // startup graph, so initialize only the read-only parser diagnostics.
    if (!cl_shownet)
        cl_shownet = Dvar_RegisterInt("cl_shownet", 0,
                                      (DvarLimits)0x4FFFFFFFELL,
                                      DVAR_NOFLAG, "Display network debugging information");
    if (!cl_shownuments)
        cl_shownuments = Dvar_RegisterBool("cl_shownuments", false,
                                           DVAR_NOFLAG, "Show the number of entities");
    if (!msg_dumpEnts)
        msg_dumpEnts = Dvar_RegisterBool("msg_dumpEnts", false,
                                         DVAR_NOFLAG, "Print snapshot entity info");
}

void VitaMpClientBridge_SubmitCommand(const usercmd_s& command) {
    clientActive_t& client = clients[0];
    const int32_t commandNumber = ++client.cmdNumber;
    client.cmds[commandNumber & 0x7F] = command;
}

void VitaMpClientBridge_PredictCommand(const usercmd_s& command) {
    if (!clients[0].snap.valid)
        return;
    // Rebase prediction on every command to the latest parsed authoritative
    // snapshot.  The Vita local session acknowledges one snapshot per frame;
    // carrying the previous ARM-mutated playerState_s into another Pmove can
    // retain transient desktop-only fields and fault on the second call.
    // Starting from the source snapshot still executes the real shared Pmove
    // path while giving it the same clean baseline the network client would
    // receive after reconciliation.
    g_predictedPlayerState = clients[0].snap.ps;
    g_predictedOldCommand = usercmd_s{};
    g_predictionValid = true;
    usercmd_s predictedCommand = command;
    if (predictedCommand.serverTime <= g_predictedPlayerState.commandTime)
        predictedCommand.serverTime = g_predictedPlayerState.commandTime + 1;
    // Bound the standalone Vita prediction step to one millisecond while the
    // authoritative snapshot cadence is being reconciled.  The source Pmove
    // loop can otherwise replay the full 33 ms gap against a partially packed
    // clSnapshot_t on ARM; server-side Pmove still receives the full command
    // interval below.
    if (predictedCommand.serverTime > g_predictedPlayerState.commandTime + 1)
        predictedCommand.serverTime = g_predictedPlayerState.commandTime + 1;
    // The authoritative server resets the real Shipment collision service at
    // the beginning of its tick.  Keep the source client prediction isolated
    // from that mutable world pointer: the compact predictor is immediately
    // reconciled by the following parsed snapshot, while server-side Pmove
    // remains the collision authority.
    VitaPmove_SetCollision(nullptr);
    VitaPmove_Run(&g_predictedPlayerState, &predictedCommand,
                  &g_predictedOldCommand);
    g_predictedOldCommand = predictedCommand;
    PublishPredictedState();
}

void VitaMpClientBridge_ApplySnapshot(const ShipmentDmPlayerState& snapshot,
                                      int32_t serverTime) {
    clientActive_t& client = clients[0];
    clSnapshot_t& frame = client.snap;
    playerState_s& player = frame.ps;

    BuildSourcePlayerState(snapshot, serverTime, &g_sourcePlayerState);
    g_sourcePlayerStateValid = true;
    player = g_sourcePlayerState;
    const int32_t weapon = WeaponIndex(snapshot.weaponSlot);
    frame.valid = snapshot.connected ? 1 : 0;
    frame.serverTime = serverTime;
    frame.messageNum += 1;
    frame.ps.commandTime = serverTime;
    frame.ps.clientNum = static_cast<int32_t>(snapshot.clientNum);
    frame.ps.pm_type = snapshot.alive ? PM_NORMAL : PM_DEAD;
    frame.ps.pm_flags &= ~(kPmfCrouched | kPmfProne | kPmfAds
                            | kPmfSprinting | kPmfJumping);
    if (snapshot.crouched)
        frame.ps.pm_flags |= kPmfCrouched;
    if (snapshot.prone)
        frame.ps.pm_flags |= kPmfProne;
    if (snapshot.ads)
        frame.ps.pm_flags |= kPmfAds;
    if (snapshot.sprinting)
        frame.ps.pm_flags |= kPmfSprinting;
    if (snapshot.jumping)
        frame.ps.pm_flags |= kPmfJumping;
    Copy3(player.origin, snapshot.origin);
    Copy3(player.velocity, snapshot.velocity);
    Copy3(player.viewangles, snapshot.angles);
    player.viewHeightCurrent = snapshot.viewHeight;
    player.viewHeightTarget = static_cast<int32_t>(snapshot.viewHeight);
    player.fWeaponPosFrac = std::clamp(snapshot.weaponPosFrac, 0.0f, 1.0f);
    player.weapon = static_cast<uint32_t>(weapon);
    player.otherFlags = POF_PLAYER;
    player.weaponstate = static_cast<weaponstate_t>(std::clamp(
        snapshot.weaponState, static_cast<int32_t>(WEAPON_READY),
        static_cast<int32_t>(WEAPON_NIGHTVISION_REMOVE)));
    player.offHandIndex = snapshot.grenadeCount > 0 ? 4 : 0;
    player.weapAnim = snapshot.weapAnim;
    player.ammoclip[weapon] = snapshot.clipAmmo;
    player.ammo[weapon] = snapshot.reserveAmmo;
    player.ammoclip[4] = std::clamp(snapshot.grenadeCount, 0, 2);
    player.ammo[4] = 0;
    player.weapons[0] = (1u << 1) | (1u << 2) | (1u << 3)
        | (snapshot.grenadeCount > 0 ? (1u << 4) : 0u);
    player.stats[0] = snapshot.health;
    client.serverTime = serverTime;
    Copy3(client.viewangles, snapshot.angles);
    clientConnections[0].serverMessageSequence += 1;
}

bool VitaMpClientBridge_CopyParsedPlayerState(playerState_s* output) {
    if (!output || !clients[0].snap.valid)
        return false;
    *output = clients[0].snap.ps;
    return true;
}

void VitaMpClientBridge_StageSnapshot(const ShipmentDmPlayerState& snapshot,
                                      int32_t serverTime) {
    BuildSourcePlayerState(snapshot, serverTime, &g_sourcePlayerState);
    g_sourcePlayerStateValid = snapshot.connected;
    // Advance the source client's acknowledged time/sequence without
    // overwriting its last parsed frame. CL_ParseSnapshot will now install
    // the serialized server result into clients[0].snap.
    clients[0].serverTime = serverTime;
    Copy3(clients[0].viewangles, snapshot.angles);
    clientConnections[0].serverMessageSequence += 1;
}

bool VitaMpClientBridge_QueueSnapshotMessage(int32_t serverTime) {
    uint8_t compressed[4096]{};
    const uint32_t compressedLength = VitaMpClientBridge_BuildSnapshotMessage(
        compressed, sizeof(compressed), serverTime);
    if (compressedLength == 0 || !g_sourceNetchanInitialized)
        return false;

    // Exercise the real source netchan header/sequence path before handing the
    // packet to the Vita-owned ring. The ring remains the platform boundary,
    // while the packet crossing it is now a native netchan packet rather than
    // a bare compressed payload.
    if (!Netchan_Transmit(&g_sourceServerNetchan, static_cast<int>(compressedLength),
                          reinterpret_cast<char*>(compressed)))
        return false;
    uint8_t packetStorage[1400]{};
    msg_t packet{};
    MSG_Init(&packet, packetStorage, static_cast<int>(sizeof(packetStorage)));
    netadr_t from{};
    if (!NET_GetLoopPacket(NS_CLIENT1, &from, &packet) || packet.cursize <= 0)
        return false;
    return VitaLoopback_SendSourceSnapshot(0, packet.data,
                                           static_cast<uint32_t>(packet.cursize));
}

bool VitaMpClientBridge_ParseSnapshotMessage(int32_t serverTime) {
    uint8_t packetStorage[1400]{};
    uint32_t compressedLength = 0;
    if (!VitaLoopback_ReceiveSourceSnapshot(0, packetStorage, sizeof(packetStorage),
                                            &compressedLength))
        return false;

    msg_t packet{};
    MSG_Init(&packet, packetStorage, static_cast<int>(sizeof(packetStorage)));
    packet.cursize = static_cast<int>(compressedLength);
    if (!Netchan_Process(&clientConnections[0].netchan, &packet)
        || packet.readcount < 0 || packet.readcount > packet.cursize)
        return false;

    // The bounded Vita session deliberately emits only svc_snapshot followed
    // by svc_EOF, so it can enter the same outer dispatcher as a desktop MP
    // packet without requiring the full gamestate/download startup graph.
    // CL_ParseServerMessage owns Huffman expansion and dispatches the packet
    // to the real CL_ParseSnapshot implementation.
    msg_t message{};
    MSG_InitReadOnly(&message,
                     packet.data + packet.readcount,
                     packet.cursize - packet.readcount);
    CL_ParseServerMessage(0, &message);
    const bool parsed = !message.overflowed
        && clients[0].snap.valid
        && clients[0].snap.serverTime == serverTime;
    if (parsed)
    {
        clientUIActives[0].connectionState = CA_ACTIVE;
        g_predictedPlayerState = clients[0].snap.ps;
        g_predictedOldCommand = clients[0].cmds[clients[0].cmdNumber & 0x7F];
        g_predictionValid = true;
        PublishPredictedState();
    }
    return parsed;
}

uint32_t VitaMpClientBridge_BuildSnapshotMessage(uint8_t* buffer,
                                                 uint32_t capacity,
                                                 int32_t serverTime) {
    if (!buffer || capacity < 4096u)
        return 0;

    // CL_ParseServerMessage consumes the Huffman-compressed payload that
    // follows the desktop netchan/reliable header. Keep the source writer's
    // packet construction separate from that transport compression so the
    // same bridge can feed either direct parser tests or the Vita loopback.
    uint8_t rawStorage[16384]{};
    msg_t message{};
    MSG_Init(&message, rawStorage, static_cast<int>(sizeof(rawStorage)));
    // The source writer is normally initialized by SV_Init. The local Vita
    // bridge has no desktop server startup, so provide the same quiet dvars
    // before exercising the writer directly.
    if (!sv_debugPacketContents)
        sv_debugPacketContents = Dvar_RegisterBool("sv_debugPacketContents", false,
                                                   DVAR_NOFLAG, "Vita packet writer diagnostics");
    if (!sv_debugPlayerstate)
        sv_debugPlayerstate = Dvar_RegisterBool("sv_debugPlayerstate", false,
                                                DVAR_NOFLAG, "Vita playerstate diagnostics");
    if (!sv_padPackets)
        sv_padPackets = Dvar_RegisterInt("sv_padPackets", 0,
                                         (DvarLimits)0x7FFFFFFF00000000LL,
                                         DVAR_NOFLAG, "Vita snapshot padding");
    PrepareNativeServerSnapshot(serverTime);
    SV_WriteSnapshotToClient(&svs.clients[0], &message);
    MSG_WriteByte(&message, 7); // svc_EOF
    if (message.overflowed)
        return 0;

    uint8_t compressedStorage[16384]{};
    const int compressed = MSG_WriteBitsCompress(
        false, rawStorage, compressedStorage, message.cursize);
    if (compressed < 0 || static_cast<uint32_t>(compressed) > capacity)
        return 0;
    std::memcpy(buffer, compressedStorage, static_cast<size_t>(compressed));
    return static_cast<uint32_t>(compressed);
}

} // namespace kisak::vita
