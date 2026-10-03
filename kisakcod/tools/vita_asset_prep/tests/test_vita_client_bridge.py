#!/usr/bin/env python3
"""Regression checks for the shared MP client-state bridge."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
CMAKE = ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt"
SOURCE = ROOT / "src" / "vita" / "vita_mp_client_bridge.cpp"
HEADER = ROOT / "src" / "vita" / "vita_mp_client_bridge.h"
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"
PARSER = ROOT / "src" / "client_mp" / "cl_parse_mp.cpp"
RUNTIME_SMOKE = ROOT / "src" / "vita" / "vita_runtime_smoke.cpp"


def main() -> int:
    cmake = CMAKE.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
    parser = PARSER.read_text(encoding="utf-8")
    runtime_smoke = RUNTIME_SMOKE.read_text(encoding="utf-8")

    assert "vita_mp_client_bridge.cpp" in cmake
    assert "VitaMpClientBridge_Initialize" in header
    assert "VitaMpClientBridge_SubmitCommand" in header
    assert "VitaMpClientBridge_PredictCommand" in header
    assert "VitaMpClientBridge_ApplySnapshot" in header
    assert "VitaMpClientBridge_CopyParsedPlayerState" in header
    assert "VitaMpClientBridge_StageSnapshot" in header
    assert "VitaMpClientBridge_ParseSnapshotMessage" in header
    assert "VitaMpClientBridge_QueueSnapshotMessage" in header
    assert "VitaMpClientBridge_BuildSnapshotMessage" in header
    assert "clientConnections[0].serverMessageSequence" in source
    assert "VitaPmove_Run" in source
    # Keep the snapshot bridge tied to KisakCOD's PMF bit assignments.  A
    # stale Vita-local bit mapping makes crouch/prone/ADS/sprint/jump disagree
    # between authoritative Pmove and the renderer.
    assert "constexpr int32_t kPmfCrouched = PMF_DUCKED;" in source
    assert "constexpr int32_t kPmfProne = PMF_PRONE;" in source
    assert "constexpr int32_t kPmfAds = PMF_SIGHT_AIMING;" in source
    assert "constexpr int32_t kPmfSprinting = PMF_SPRINTING;" in source
    assert "constexpr int32_t kPmfJumping = PMF_JUMPING;" in source
    assert "player->weaponstate" in source
    assert "player->ammoclip[4]" in source
    assert "g_predictedPlayerState" in source
    assert "player.ammoclip[weapon]" in source
    assert "SV_WriteSnapshotToClient" in source
    assert "g_sourcePlayerStateValid" in source
    assert "PrepareNativeServerSnapshot" in source
    assert "svs.clients[0]" in source
    assert "svsHeaderValid" in source
    assert "MSG_WriteBitsCompress" in source
    assert "CL_ParseSnapshot" in source
    assert "CL_ParseServerMessage" in source
    assert "Netchan_Init" in source
    assert "Netchan_Setup" in source
    assert "Netchan_Transmit" in source
    assert "Netchan_Process" in source
    assert "NET_GetLoopPacket" in source
    assert "VitaLoopback_SendSourceSnapshot" in source
    assert "CA_CONNECTED" in source and "CA_ACTIVE" in source
    assert "clientUIActives" in source
    assert "VitaMpClientBridge_StageSnapshot" in bootstrap
    assert "VitaMpClientBridge_ParseSnapshotMessage" in bootstrap
    assert "VitaMpClientBridge_CopyParsedPlayerState" in bootstrap
    assert '"renderer_source_state", "cl_snapshot_playerstate"' in bootstrap
    assert "src/client_mp/cl_parse_mp.cpp" in cmake
    assert "KISAK_VITA_SLICE" in parser
    assert "case svc_snapshot" in parser
    assert "vita_snapshot_parser_services.cpp" in cmake
    assert "KisakCOD-vita-snapshot-writer-probe" in cmake
    assert "KisakCOD-vita-snapshot-parser-probe" in cmake
    # The cross-compiled smoke gate must retain an authoritative two-client
    # combat qualification alongside the one-client production bridge.
    assert "ShipmentDmServer smokeServer" in runtime_smoke
    assert "ShipmentDmEventType::Damage" in runtime_smoke
    assert "ShipmentDmEventType::Death" in runtime_smoke
    assert "ShipmentDmEventType::Kill" in runtime_smoke
    assert "ShipmentDmEventType::Respawn" in runtime_smoke
    assert "ElapsedSince" in runtime_smoke or "ElapsedSince" in (
        ROOT / "src" / "vita" / "vita_shipment_dm.cpp"
    ).read_text(encoding="utf-8")
    assert "unlimitedRules.timeLimitMsec = 0" in runtime_smoke
    assert "unlimitedMatch.elapsedMsec != 2500" in runtime_smoke
    print("vita client bridge tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
