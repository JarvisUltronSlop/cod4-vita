#include <cstdint>
#include <cmath>
#include <cstring>

#include <bgame/bg_local.h>
#include <bgame/bg_weapon_def_vita.h>
#include <qcommon/net_chan_mp.h>
#include <vita/kvap_format.h>
#include <vita/vita_asset_db.h>
#include <vita/vita_audio.h>
#include <vita/vita_animation_bundle.h>
#include <vita/vita_frontend.h>
#include <vita/vita_hud_bundle.h>
#include <vita/vita_input.h>
#include <vita/vita_net_compat.h>
#include <vita/vita_pmove_services.h>
#include <vita/vita_shipment_dm.h>

namespace {

struct MemoryPackage {
    // Keep the fixture self-contained while leaving room for the compact
    // collision/world chunks used by the authoritative DM qualification
    // below.  This never reads from or writes to the installed game assets.
    uint8_t bytes[8192]{};
    size_t size = 0;
};

bool ReadMemory(void* rawContext, uint64_t offset, void* destination, size_t size) {
    auto* package = static_cast<MemoryPackage*>(rawContext);
    if (!package || offset > package->size || size > package->size - offset)
        return false;
    std::memcpy(destination, package->bytes + offset, size);
    return true;
}

MemoryPackage MakePackage() {
    MemoryPackage package{};
    const uint8_t payload[] = { 'v', 'i', 't', 'a', '-', 's', 'm', 'o', 'k', 'e' };
    const char role[] = "weapon_m4";
    const char image[] = "hud_m4carbine";
    const char ammoRole[] = "ammo_rifle";
    const char ammoImage[] = "ammo_counter_riflebullet";
    uint8_t hudPayload[128]{};
    const uint32_t hudMagic = 0x48544956; // "VITH"
    const uint32_t hudVersion = 1;
    const uint32_t hudRecords = 2;
    const uint64_t hudDataBytes = (8 + sizeof(role) - 1 + sizeof(image) - 1)
        + (8 + sizeof(ammoRole) - 1 + sizeof(ammoImage) - 1);
    std::memcpy(hudPayload + 0, &hudMagic, sizeof(hudMagic));
    std::memcpy(hudPayload + 4, &hudVersion, sizeof(hudVersion));
    std::memcpy(hudPayload + 8, &hudRecords, sizeof(hudRecords));
    std::memcpy(hudPayload + 12, &hudDataBytes, sizeof(hudDataBytes));
    const uint32_t roleBytes = sizeof(role) - 1;
    const uint32_t imageBytes = sizeof(image) - 1;
    std::memcpy(hudPayload + 20, &roleBytes, sizeof(roleBytes));
    std::memcpy(hudPayload + 24, &imageBytes, sizeof(imageBytes));
    std::memcpy(hudPayload + 28, role, roleBytes);
    std::memcpy(hudPayload + 28 + roleBytes, image, imageBytes);
    const size_t secondOffset = 28 + roleBytes + imageBytes;
    const uint32_t ammoRoleBytes = sizeof(ammoRole) - 1;
    const uint32_t ammoImageBytes = sizeof(ammoImage) - 1;
    std::memcpy(hudPayload + secondOffset, &ammoRoleBytes, sizeof(ammoRoleBytes));
    std::memcpy(hudPayload + secondOffset + 4, &ammoImageBytes, sizeof(ammoImageBytes));
    std::memcpy(hudPayload + secondOffset + 8, ammoRole, ammoRoleBytes);
    std::memcpy(hudPayload + secondOffset + 8 + ammoRoleBytes,
                ammoImage, ammoImageBytes);
    const uint32_t hudSize = static_cast<uint32_t>(secondOffset + 8
                                                   + ammoRoleBytes + ammoImageBytes);
    uint8_t animationPayload[512]{};
    const uint32_t animationMagic = 0x51544956; // "VITQ"
    const uint32_t animationVersion = 2;
    const uint32_t animationRecords = 1;
    const uint32_t animationRecordBytes = 162;
    std::memcpy(animationPayload + 0, &animationMagic, sizeof(animationMagic));
    std::memcpy(animationPayload + 4, &animationVersion, sizeof(animationVersion));
    std::memcpy(animationPayload + 8, &animationRecords, sizeof(animationRecords));
    std::memcpy(animationPayload + 12, &animationRecordBytes,
                sizeof(animationRecordBytes));
    uint8_t* animationRecord = animationPayload + 16;
    const char animationName[] = "viewmodel_mp5_reload";
    std::memcpy(animationRecord, animationName, sizeof(animationName) - 1);
    const uint16_t animationFrames = 4;
    std::memcpy(animationRecord + 128, &animationFrames, sizeof(animationFrames));
    animationRecord[130] = 0; // non-looping
    animationRecord[142] = 2; // clip event plus end
    const uint32_t animationNotifyBytes =
        8u + static_cast<uint32_t>(sizeof("weap_mp5_clipin_plr") - 1)
        + 8u + static_cast<uint32_t>(sizeof("end") - 1);
    const uint32_t animationTrackCount = 0;
    std::memcpy(animationRecord + 146, &animationTrackCount,
                sizeof(animationTrackCount));
    std::memcpy(animationRecord + 150, &animationNotifyBytes,
                sizeof(animationNotifyBytes));
    const float animationRate = 30.0f;
    std::memcpy(animationRecord + 154, &animationRate, sizeof(animationRate));
    std::memcpy(animationRecord + 158, &animationRate, sizeof(animationRate));
    size_t animationOffset = 16 + animationRecordBytes;
    const uint32_t clipNotifyNameBytes =
        static_cast<uint32_t>(sizeof("weap_mp5_clipin_plr") - 1);
    std::memcpy(animationPayload + animationOffset, &clipNotifyNameBytes,
                sizeof(clipNotifyNameBytes));
    const float clipNotifyTime = 0.5f;
    std::memcpy(animationPayload + animationOffset + 4, &clipNotifyTime,
                sizeof(clipNotifyTime));
    animationOffset += 8;
    std::memcpy(animationPayload + animationOffset, "weap_mp5_clipin_plr",
                clipNotifyNameBytes);
    animationOffset += clipNotifyNameBytes;
    const uint32_t endNotifyNameBytes = static_cast<uint32_t>(sizeof("end") - 1);
    std::memcpy(animationPayload + animationOffset, &endNotifyNameBytes,
                sizeof(endNotifyNameBytes));
    const float endNotifyTime = 1.0f;
    std::memcpy(animationPayload + animationOffset + 4, &endNotifyTime,
                sizeof(endNotifyTime));
    animationOffset += 8;
    std::memcpy(animationPayload + animationOffset, "end", endNotifyNameBytes);
    animationOffset += endNotifyNameBytes;
    const uint32_t animationSize = static_cast<uint32_t>(animationOffset);
    auto* header = reinterpret_cast<kisak::vita::KvapHeader*>(package.bytes);
    std::memcpy(header->magic, "KVAP", 4);
    header->major = kisak::vita::kKvapMajor;
    header->minor = kisak::vita::kKvapMinor;
    header->endian = kisak::vita::kKvapLittleEndian;
    header->headerSize = sizeof(kisak::vita::KvapHeader);
    header->tocOffset = sizeof(kisak::vita::KvapHeader);
    header->tocCount = 3;
    auto* chunk = reinterpret_cast<kisak::vita::KvapChunk*>(
        package.bytes + sizeof(kisak::vita::KvapHeader));
    chunk->type = static_cast<uint32_t>(kisak::vita::KvapChunkType::Manifest);
    chunk->assetId = 0x1234;
    chunk->offset = 256;
    chunk->storedSize = sizeof(payload);
    chunk->rawSize = sizeof(payload);
    chunk->alignment = 16;
    chunk->codec = static_cast<uint32_t>(kisak::vita::KvapCodec::None);
    chunk->crc32 = kisak::vita::KvapCrc32(payload, sizeof(payload));
    std::memcpy(package.bytes + chunk->offset, payload, sizeof(payload));
    auto* hudChunk = reinterpret_cast<kisak::vita::KvapChunk*>(
        package.bytes + sizeof(kisak::vita::KvapHeader) + sizeof(kisak::vita::KvapChunk));
    hudChunk->type = static_cast<uint32_t>(kisak::vita::KvapChunkType::Hud);
    hudChunk->assetId = 0x5678;
    hudChunk->offset = 512;
    hudChunk->storedSize = hudSize;
    hudChunk->rawSize = hudSize;
    hudChunk->alignment = 16;
    hudChunk->codec = static_cast<uint32_t>(kisak::vita::KvapCodec::None);
    hudChunk->crc32 = kisak::vita::KvapCrc32(hudPayload, hudSize);
    std::memcpy(package.bytes + hudChunk->offset, hudPayload, hudSize);
    auto* animationChunk = reinterpret_cast<kisak::vita::KvapChunk*>(
        package.bytes + sizeof(kisak::vita::KvapHeader)
            + sizeof(kisak::vita::KvapChunk) * 2);
    animationChunk->type = static_cast<uint32_t>(kisak::vita::KvapChunkType::Animation);
    animationChunk->assetId = 0x9abc;
    animationChunk->offset = 1024;
    animationChunk->storedSize = animationSize;
    animationChunk->rawSize = animationSize;
    animationChunk->alignment = 16;
    animationChunk->codec = static_cast<uint32_t>(kisak::vita::KvapCodec::None);
    animationChunk->crc32 = kisak::vita::KvapCrc32(animationPayload, animationSize);
    std::memcpy(package.bytes + animationChunk->offset, animationPayload, animationSize);

    // A minimal but valid VITC header is enough to exercise the server's
    // world-trace path; no brush is needed for this deterministic combat
    // check.  The real map package uses the same pointer-free sidecar format.
    constexpr uint64_t collisionOffset = 1536;
    const uint32_t collisionHeader[] = {0x43544956u, 2u, 0u, 0u, 0u, 0u, 0u, 0u};
    auto* collisionChunk = reinterpret_cast<kisak::vita::KvapChunk*>(
        package.bytes + sizeof(kisak::vita::KvapHeader)
            + sizeof(kisak::vita::KvapChunk) * 3);
    collisionChunk->type = static_cast<uint32_t>(kisak::vita::KvapChunkType::Collision);
    collisionChunk->assetId = 0xc0111deu;
    collisionChunk->offset = collisionOffset;
    collisionChunk->storedSize = sizeof(collisionHeader);
    collisionChunk->rawSize = sizeof(collisionHeader);
    collisionChunk->alignment = 16;
    collisionChunk->codec = static_cast<uint32_t>(kisak::vita::KvapCodec::None);
    collisionChunk->crc32 = kisak::vita::KvapCrc32(collisionHeader, sizeof(collisionHeader));
    std::memcpy(package.bytes + collisionOffset, collisionHeader, sizeof(collisionHeader));

    // Two opposing spawns make the hitscan check independent of the authored
    // Shipment entity export while retaining the exact VITE parser contract.
    constexpr char worldText[] =
        "{ \"classname\" \"mp_dm_spawn\" \"origin\" \"0 0 0\" \"angles\" \"0 0 0\" \"gndLt\" \"3322114407\" }\n"
        "{ \"classname\" \"mp_dm_spawn\" \"origin\" \"96 0 0\" \"angles\" \"0 180 0\" }\n";
    const uint32_t worldHeader[] = {
        0x45544956u, 1u, static_cast<uint32_t>(sizeof(worldText) - 1),
    };
    constexpr uint64_t worldOffset = 2048;
    auto* worldChunk = reinterpret_cast<kisak::vita::KvapChunk*>(
        package.bytes + sizeof(kisak::vita::KvapHeader)
            + sizeof(kisak::vita::KvapChunk) * 4);
    worldChunk->type = static_cast<uint32_t>(kisak::vita::KvapChunkType::World);
    worldChunk->assetId = 0x574f524c44u;
    worldChunk->offset = worldOffset;
    worldChunk->storedSize = sizeof(worldHeader) + sizeof(worldText) - 1;
    worldChunk->rawSize = worldChunk->storedSize;
    worldChunk->alignment = 16;
    worldChunk->codec = static_cast<uint32_t>(kisak::vita::KvapCodec::None);
    std::memcpy(package.bytes + worldOffset, worldHeader, sizeof(worldHeader));
    std::memcpy(package.bytes + worldOffset + sizeof(worldHeader),
                worldText, sizeof(worldText) - 1);
    worldChunk->crc32 = kisak::vita::KvapCrc32(package.bytes + worldOffset,
                                                worldChunk->rawSize);

    header->tocCount = 5;
    package.size = static_cast<size_t>(worldChunk->offset + worldChunk->storedSize);
    return package;
}

} // namespace

// Link-time gate for the selected engine archive.  This intentionally calls a
// real shared Pmove helper, not a duplicate test implementation; if the
// gameplay object grows an unresolved desktop dependency the Vita runtime
// target fails here instead of silently shipping an object-only probe.
int main()
{
    MemoryPackage package = MakePackage();
    kisak::vita::VitaAssetDatabase database;
    if (database.Mount(ReadMemory, &package, package.size) != kisak::vita::KvapError::Ok)
        return 2;
    const auto* manifest = database.Find(kisak::vita::VitaAssetType::Manifest, 0x1234);
    if (database.IsPlayableShipment())
        return 9;
    kisak::vita::VitaHudBundle hud;
    if (!hud.Load(database) || hud.Count() != 2
        || !hud.ImageFor("weapon_m4")
        || std::strcmp(hud.ImageFor("weapon_m4"), "hud_m4carbine") != 0
        || !hud.ImageFor("ammo_rifle")
        || std::strcmp(hud.ImageFor("ammo_rifle"), "ammo_counter_riflebullet") != 0
        || hud.ImageFor("missing") != nullptr)
        return 22;
    kisak::vita::VitaAnimationBundle animations;
    if (!animations.Load(database) || animations.Count() != 1
        || animations.NotifyCount() != 2)
        return 24;
    const auto* reload = animations.Find("viewmodel_mp5_reload");
    const kisak::vita::VitaAnimationNotify* crossed[2]{};
    if (!reload || animations.CollectNotifies(reload, 0.0f, 0.75f, false,
                                               crossed, 2) != 1
        || !crossed[0] || crossed[0]->name != "weap_mp5_clipin_plr")
        return 25;
    uint8_t bytes[16]{};
    if (!manifest || database.Read(manifest, bytes, sizeof(bytes)) != kisak::vita::KvapError::Ok
        || std::memcmp(bytes, "vita-smoke", 10) != 0)
        return 3;
    uint8_t rangeBytes[4]{};
    if (database.ReadRange(manifest, 2, rangeBytes, sizeof(rangeBytes))
            != kisak::vita::KvapError::Ok
        || std::memcmp(rangeBytes, "ta-s", sizeof(rangeBytes)) != 0)
        return 16;

    VitaInputSample sampleInput{};
    sampleInput.forwardmove = 32;
    sampleInput.buttons = BUTTON_ATTACK;
    sampleInput.lookX = 127;
    sampleInput.viewAngles[0] = -364;
    sampleInput.viewAngles[1] = 728;
    usercmd_s command{};
    VitaInput_ApplyUsercmd(&sampleInput, 100, &command);
    if (command.serverTime != 100 || command.forwardmove != 32
        || (command.buttons & BUTTON_ATTACK) == 0
        || command.angles[0] != sampleInput.viewAngles[0]
        || command.angles[1] != sampleInput.viewAngles[1])
        return 4;

    // Four rear zones are isolated by dead margins: upper-left frag,
    // upper-right special, lower-left held sprint, lower-right edge melee.
    // Held edge actions must not retrigger on alternating polls.
    VitaInputDevice rearTouch{};
    rearTouch.backTouch.reportNum = 1;
    rearTouch.backTouch.report[0].x = 1600;
    rearTouch.backTouch.report[0].y = 800;
    VitaInputSample rearSample{};
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    if (!rearSample.rearMeleePressed || (rearSample.buttons & BUTTON_MELEE) == 0)
        return 13;
    rearSample = VitaInputSample{};
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    if (rearSample.rearMeleePressed || (rearSample.buttons & BUTTON_MELEE) != 0)
        return 14;
    rearTouch.backTouch.reportNum = 0;
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    rearTouch.backTouch.reportNum = 1;
    rearTouch.backTouch.report[0].x = 300;
    rearTouch.backTouch.report[0].y = 800;
    rearSample = VitaInputSample{};
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    if (!rearSample.rearSprintHeld || (rearSample.buttons & BUTTON_SPRINT) == 0)
        return 15;
    rearTouch.backTouch.reportNum = 0;
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    rearTouch.backTouch.reportNum = 1;
    rearTouch.backTouch.report[0].x = 300;
    rearTouch.backTouch.report[0].y = 240;
    rearSample = VitaInputSample{};
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    if (!rearSample.rearFragPressed || (rearSample.buttons & BUTTON_FRAG) == 0)
        return 26;
    rearSample = {};
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    if (rearSample.rearFragPressed || !rearSample.rearFragHeld
        || (rearSample.buttons & BUTTON_FRAG) == 0)
        return 66;
    rearTouch.backTouch.reportNum = 0;
    rearSample = {};
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    if (rearSample.rearFragHeld || (rearSample.buttons & BUTTON_FRAG) != 0)
        return 67;
    rearTouch.backTouch.reportNum = 1;
    rearTouch.backTouch.report[0].x = 1600;
    rearTouch.backTouch.report[0].y = 240;
    rearSample = VitaInputSample{};
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    if (!rearSample.rearSpecialPressed || (rearSample.buttons & BUTTON_SMOKE) == 0)
        return 27;
    rearSample = {};
    VitaInput_ApplyRearTouch(&rearTouch, true, &rearSample);
    if (rearSample.rearSpecialPressed || !rearSample.rearSpecialHeld
        || (rearSample.buttons & BUTTON_SMOKE) == 0)
        return 68;
    rearSample = {};
    VitaInput_ApplyRearTouch(&rearTouch, false, &rearSample);
    if (rearSample.rearSpecialHeld || rearSample.buttons != 0)
        return 69;

    uint8_t packet[4] = { 0xCA, 0xFE, 0xBA, 0xBE };
    netadr_t loopAddress{};
    loopAddress.type = NA_LOOPBACK;
    loopAddress.port = NS_SERVER;
    NET_SendLoopPacket(NS_CLIENT1, sizeof(packet), packet, loopAddress);
    uint8_t receiveBuffer[16]{};
    msg_t message{};
    message.data = receiveBuffer;
    message.maxsize = sizeof(receiveBuffer);
    netadr_t from{};
    if (!NET_GetLoopPacket_Real(NS_SERVER, &from, &message)
        || message.cursize != sizeof(packet)
        || std::memcmp(receiveBuffer, packet, sizeof(packet)) != 0
        || from.type != NA_LOOPBACK)
        return 5;

    // Exercise the Vita-local client-to-server queue independently of the
    // shared net_chan ring.  This keeps the one-client bridge packet-shaped
    // and bounded instead of allowing a direct state pointer to return.
    VitaLoopback_Reset();
    const uint8_t commandPacket[] = { 'D', 'M', 'C', '1', 0x10, 0x20, 0x30, 0x40 };
    uint8_t commandReceive[sizeof(commandPacket)]{};
    uint32_t commandLength = 0;
    if (!VitaLoopback_SendCommand(0, commandPacket, sizeof(commandPacket))
        || !VitaLoopback_ReceiveCommand(0, commandReceive, sizeof(commandReceive),
                                         &commandLength)
        || commandLength != sizeof(commandPacket)
        || std::memcmp(commandReceive, commandPacket, sizeof(commandPacket)) != 0)
        return 6;

    uint8_t wire[16]{};
    msg_t writeMessage{};
    writeMessage.data = wire;
    writeMessage.maxsize = sizeof(wire);
    MSG_WriteLong(&writeMessage, 0x13572468);
    msg_t readMessage{};
    readMessage.readOnly = 1;
    readMessage.data = wire;
    readMessage.maxsize = writeMessage.cursize;
    readMessage.cursize = writeMessage.cursize;
    MSG_BeginReading(&readMessage);
    if (MSG_ReadLong(&readMessage) != 0x13572468)
        return 7;

    const float velocityInput[3] = { 1.0f, -0.5f, 0.25f };
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    float output[3]{};
    PM_ClipVelocity(velocityInput, normal, output);
    if (output[2] != 0.0f)
        return 8;
    float projected[3]{};
    PM_ProjectVelocity(velocityInput, normal, projected);
    if (!(projected[2] == 0.0f && projected[0] > 1.0f))
        return 9;

    VitaPmoveWeaponConfig weaponConfig{};
    weaponConfig.weapon = 1;
    weaponConfig.fireType = WEAPON_FIRETYPE_FULLAUTO;
    weaponConfig.moveSpeedScale = 0.75f;
    weaponConfig.adsMoveSpeedScale = 0.50f;
    VitaPmove_SetWeaponConfig(&weaponConfig);
    const WeaponDef* configuredWeapon = BG_GetWeaponDef(1);
    if (!configuredWeapon || std::fabs(configuredWeapon->moveSpeedScale - 0.75f) > 0.0001f
        || std::fabs(configuredWeapon->adsMoveSpeedScale - 0.50f) > 0.0001f
        || configuredWeapon->fireType != WEAPON_FIRETYPE_FULLAUTO)
        return 23;

    playerState_s movementState{};
    movementState.pm_type = PM_NORMAL;
    movementState.commandTime = 0;
    movementState.groundEntityNum = ENTITYNUM_NONE;
    movementState.viewHeightTarget = 60;
    movementState.viewHeightCurrent = 60.0f;
    movementState.speed = 190;
    movementState.gravity = 800;
    usercmd_s movementCommand{};
    movementCommand.serverTime = 16;
    movementCommand.forwardmove = 127;
    movementCommand.rightmove = 64;
    const float commandScale = PM_CmdScale(&movementState, &movementCommand);
    if (!(commandScale > 0.0f && commandScale < 191.0f))
        return 11;

    // Execute one frame through the shared multiplayer Pmove entry point.
    // With no mounted collision package this follows the same bounded empty
    // trace path used by the ARM link probe, while still proving that the
    // Vita-owned dvar/trace/weapon services satisfy the real call graph.
    VitaPmove_SetCollision(nullptr);
    if (!VitaPmove_Run(&movementState, &movementCommand, nullptr)
        || movementState.commandTime != movementCommand.serverTime
        || !std::isfinite(movementState.origin[0])
        || !std::isfinite(movementState.origin[1])
        || !std::isfinite(movementState.origin[2]))
        return 12;

    // Drive the real authoritative Shipment server with two local
    // free-for-all clients.  The fixture uses valid VITC/VITE payloads above, so this
    // covers the same spawn -> Pmove -> hitscan -> damage/death -> score ->
    // respawn path as production without depending on host filesystem data.
    kisak::vita::ShipmentCollision smokeCollision;
    kisak::vita::ShipmentMapEntities smokeEntities;
    if (!smokeCollision.Load(database) || !smokeEntities.Load(database)
        || smokeEntities.DeathmatchSpawns().size() < 2)
        return 28;
    if (smokeEntities.Records().empty()
        || !smokeEntities.Records()[0].hasGroundLighting
        || smokeEntities.Records()[0].groundLighting != 0x44332211u
        || smokeEntities.Records()[0].groundPrimaryLightIndex != 7u)
        return 37;

    kisak::vita::ShipmentDmServer smokeServer;
    if (!smokeServer.Initialize(&smokeCollision, &smokeEntities, nullptr, 2))
        return 29;
    if (smokeServer.GameMode() != kisak::vita::ShipmentDmGameMode::Deathmatch
        || smokeServer.MatchTimeLimitMsec() != kisak::vita::kShipmentDmTimeLimitMsec
        || smokeServer.MatchScoreLimit() != kisak::vita::kShipmentDmScoreLimit
        || smokeServer.MatchKillScore() != kisak::vita::kShipmentDmKillScore
        || smokeServer.MatchPhase() != kisak::vita::ShipmentDmMatchPhase::Waiting
        || smokeServer.MatchServerTime() != 0)
        return 40;
    kisak::vita::ShipmentDmClient smokeClient;
    if (!smokeClient.Connect(&smokeServer))
        return 30;
    const uint32_t shooterClient = smokeClient.ClientNum();
    const uint32_t targetClient = smokeServer.ConnectLocalClient();
    if (shooterClient == UINT32_MAX || targetClient == UINT32_MAX
        || shooterClient == targetClient)
        return 30;
    smokeServer.Tick(0);
    const auto* spawnedShooter = smokeServer.Player(shooterClient);
    const auto* spawnedTarget = smokeServer.Player(targetClient);
    if (!spawnedShooter || !spawnedTarget || !spawnedShooter->alive
        || !spawnedTarget->alive || spawnedShooter->team != 0
        || spawnedTarget->team != 0
        || smokeServer.MatchPhase() != kisak::vita::ShipmentDmMatchPhase::Active
        || smokeServer.MatchServerTime() != 0)
        return 31;
    const auto* clientSpawnSnapshot = smokeClient.Snapshot();
    kisak::vita::ShipmentDmMatchState clientMatchState{};
    if (!clientSpawnSnapshot || !smokeClient.CopyMatchState(&clientMatchState)
        || clientMatchState.mode != kisak::vita::ShipmentDmGameMode::Deathmatch
        || clientMatchState.phase != kisak::vita::ShipmentDmMatchPhase::Active
        || clientMatchState.elapsedMsec != 0
        || clientMatchState.timeLimitMsec != kisak::vita::kShipmentDmTimeLimitMsec
        || clientMatchState.scoreLimit != kisak::vita::kShipmentDmScoreLimit
        || clientMatchState.killScore != kisak::vita::kShipmentDmKillScore
        || clientMatchState.winnerClientNum != UINT32_MAX)
        return 43;
    smokeServer.ClearEvents();

    usercmd_s shotCommand{};
    shotCommand.serverTime = 100;
    shotCommand.weapon = 1;
    shotCommand.buttons = BUTTON_ATTACK;
    if (!smokeClient.Frame(shotCommand))
        return 32;
    smokeServer.Tick(100);
    if (smokeServer.MatchServerTime() != 100)
        return 41;
    if (!smokeClient.CopyMatchState(&clientMatchState)
        || clientMatchState.phase != kisak::vita::ShipmentDmMatchPhase::Active
        || clientMatchState.elapsedMsec != 100)
        return 44;
    const auto* hitTarget = smokeServer.Player(targetClient);
    if (!hitTarget || hitTarget->health >= 100)
        return 33;
    bool sawFire = false;
    bool sawDamage = false;
    for (const auto& event : smokeServer.Events()) {
        sawFire = sawFire || event.type == kisak::vita::ShipmentDmEventType::Fire;
        sawDamage = sawDamage || event.type == kisak::vita::ShipmentDmEventType::Damage;
    }
    if (!sawFire || !sawDamage)
        return 34;

    smokeServer.ClearEvents();
    const float impactPoint[3] = {96.0f, 0.0f, 60.0f};
    const float impactDirection[3] = {1.0f, 0.0f, 0.0f};
    if (!smokeServer.ApplyDamage(targetClient, shooterClient, 200, 200, 1,
                                 impactPoint, impactDirection))
        return 35;
    const auto* deadTarget = smokeServer.Player(targetClient);
    const auto* scoredShooter = smokeServer.Player(shooterClient);
    if (!deadTarget || deadTarget->alive || deadTarget->deaths != 1
        || !scoredShooter || scoredShooter->score != kisak::vita::kShipmentDmKillScore)
        return 36;
    if (smokeServer.Events().size() != 3
        || smokeServer.Events()[0].type != kisak::vita::ShipmentDmEventType::Damage
        || smokeServer.Events()[1].type != kisak::vita::ShipmentDmEventType::Death
        || smokeServer.Events()[2].type != kisak::vita::ShipmentDmEventType::Kill)
        return 37;
    smokeServer.Tick(1200);
    const auto* respawnedTarget = smokeServer.Player(targetClient);
    if (!respawnedTarget || !respawnedTarget->alive || respawnedTarget->health != 100
        || respawnedTarget->respawnTime != 0)
        return 38;
    if (smokeServer.Events().empty()
        || smokeServer.Events().back().type != kisak::vita::ShipmentDmEventType::Respawn)
        return 39;
    // The authored dm.gsc ends a match at the configured time limit.  Verify
    // the server freezes gameplay at that boundary and exposes the winning
    // client for the intermission/scoreboard presentation.
    smokeServer.Tick(kisak::vita::kShipmentDmTimeLimitMsec);
    if (!smokeServer.MatchFinished()
        || smokeServer.MatchPhase() != kisak::vita::ShipmentDmMatchPhase::Complete
        || smokeServer.MatchServerTime() != kisak::vita::kShipmentDmTimeLimitMsec
        || smokeServer.MatchWinnerClientNum() != shooterClient)
        return 42;
    if (!smokeClient.CopyMatchState(&clientMatchState)
        || clientMatchState.phase != kisak::vita::ShipmentDmMatchPhase::Complete
        || clientMatchState.elapsedMsec != kisak::vita::kShipmentDmTimeLimitMsec
        || clientMatchState.scoreLimit != kisak::vita::kShipmentDmScoreLimit
        || clientMatchState.killScore != kisak::vita::kShipmentDmKillScore
        || clientMatchState.winnerClientNum != shooterClient)
        return 45;

    // Exercise the typed rules injection independently of the retail defaults.
    // A zero score limit is the source convention for an unlimited playlist;
    // a kill must still award the authored custom value and the short custom
    // time limit must still drive the normal intermission transition.
    kisak::vita::ShipmentDmRules customRules{};
    customRules.timeLimitMsec = 2000;
    customRules.scoreLimit = 0;
    customRules.killScore = 7;
    kisak::vita::ShipmentDmServer customServer;
    if (!customServer.Initialize(&smokeCollision, &smokeEntities, nullptr, 2,
                                 &customRules)
        || customServer.MatchTimeLimitMsec() != 2000
        || customServer.MatchScoreLimit() != 0
        || customServer.MatchKillScore() != 7)
        return 46;
    const uint32_t customShooter = customServer.ConnectLocalClient();
    const uint32_t customTarget = customServer.ConnectLocalClient();
    if (customShooter == UINT32_MAX || customTarget == UINT32_MAX)
        return 47;
    customServer.Tick(0);
    if (!customServer.ApplyDamage(customTarget, customShooter, 200, 100)
        || !customServer.Player(customShooter)
        || customServer.Player(customShooter)->score != 7
        || customServer.MatchPhase() != kisak::vita::ShipmentDmMatchPhase::Active)
        return 48;
    customServer.Tick(2000);
    if (customServer.MatchPhase() != kisak::vita::ShipmentDmMatchPhase::Complete
        || customServer.MatchWinnerClientNum() != customShooter)
        return 49;

    // registerTimeLimitDvar also accepts zero for an unlimited playlist.  The
    // client metadata lane must preserve that start-relative elapsed clock
    // instead of clamping every packet back to zero, while a zero score limit
    // remains unlimited as exercised above.
    kisak::vita::ShipmentDmRules unlimitedRules{};
    unlimitedRules.timeLimitMsec = 0;
    unlimitedRules.scoreLimit = 0;
    unlimitedRules.killScore = 5;
    kisak::vita::ShipmentDmServer unlimitedServer;
    if (!unlimitedServer.Initialize(&smokeCollision, &smokeEntities, nullptr, 1,
                                    &unlimitedRules))
        return 50;
    kisak::vita::ShipmentDmClient unlimitedClient;
    if (!unlimitedClient.Connect(&unlimitedServer))
        return 51;
    unlimitedServer.Tick(0);
    unlimitedServer.Tick(2500);
    if (unlimitedServer.MatchPhase() != kisak::vita::ShipmentDmMatchPhase::Active
        || unlimitedServer.MatchServerTime() != 2500)
        return 52;
    kisak::vita::ShipmentDmMatchState unlimitedMatch{};
    if (!unlimitedClient.CopyMatchState(&unlimitedMatch)
        || unlimitedMatch.phase != kisak::vita::ShipmentDmMatchPhase::Active
        || unlimitedMatch.timeLimitMsec != 0
        || unlimitedMatch.elapsedMsec != 2500)
        return 53;

    // Qualify the real bounded mixer without opening SceAudio. This catches
    // the two hardware-relevant cases that are easy to regress while wiring
    // sound aliases: 24 kHz mono resampling/duplication and deterministic
    // saturation when several voices overlap.
    const int16_t resampleSamples[] = {1000, 3000};
    VitaAudioDevice mixer{};
    mixer.frames = 4;
    mixer.voices[0].samples = resampleSamples;
    mixer.voices[0].frames = 2;
    mixer.voices[0].channels = 1;
    mixer.voices[0].rate = 24000;
    mixer.voices[0].active = true;
    if (!VitaAudio_MixVoices(&mixer)
        || mixer.silence[0] != 1000 || mixer.silence[1] != 1000
        || mixer.silence[2] != 2000 || mixer.silence[3] != 2000
        || mixer.silence[4] != 3000 || mixer.silence[5] != 3000
        || mixer.voices[0].active)
        return 26;
    const int16_t loudSamples[] = {24000};
    VitaAudio_StopAll(&mixer);
    mixer.frames = 64;
    for (uint32_t voiceIndex = 0; voiceIndex < 2; ++voiceIndex) {
        mixer.voices[voiceIndex].samples = loudSamples;
        mixer.voices[voiceIndex].frames = 1;
        mixer.voices[voiceIndex].channels = 1;
        mixer.voices[voiceIndex].rate = 48000;
        mixer.voices[voiceIndex].active = true;
    }
    if (!VitaAudio_MixVoices(&mixer)
        || mixer.silence[0] != 32767 || mixer.silence[1] != 32767)
        return 27;

    // Exercise the controller-only COD4 capability route without a renderer:
    // Main -> Multiplayer -> Deathmatch -> Shipment -> Loading -> InGame.
    // This keeps the route contract testable on ARM even when Vita3K cannot
    // inject physical controller events into the installed title.
    kisak::vita::VitaFrontend frontend;
    frontend.Reset();
    VitaInputSample menuInput{};
    menuInput.menuAcceptPressed = true;
    if (!frontend.HandleInput(menuInput, true)
        || frontend.Screen() != kisak::vita::VitaFrontendScreen::Multiplayer)
        return 17;
    if (!frontend.HandleInput(menuInput, true)
        || frontend.Screen() != kisak::vita::VitaFrontendScreen::GameModeSelect
        || !frontend.IsDeathmatch())
        return 18;
    if (!frontend.HandleInput(menuInput, true)
        || frontend.Screen() != kisak::vita::VitaFrontendScreen::MapSelect
        || !frontend.IsDeathmatch())
        return 19;
    if (!frontend.HandleInput(menuInput, false)
        || frontend.Screen() != kisak::vita::VitaFrontendScreen::MapSelect)
        return 20;
    if (!frontend.HandleInput(menuInput, true)
        || frontend.Screen() != kisak::vita::VitaFrontendScreen::Loading)
        return 21;
    for (int frame = 0; frame < 6; ++frame)
        frontend.Update();
    if (!frontend.InGame())
        return 22;
    return 0;
}
