"""Static ownership and timing checks for the real source loopback session."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / "src" / "vita" / "vita_source_session.h"
SOURCE = ROOT / "src" / "vita" / "vita_source_session.cpp"
SERVICES = ROOT / "src" / "vita" / "vita_script_services.cpp"
CLIENT = ROOT / "src" / "client_mp" / "cl_main_mp.cpp"
PARSER = ROOT / "src" / "client_mp" / "cl_parse_mp.cpp"
INPUT = ROOT / "src" / "client_mp" / "cl_input.cpp"
CGAME = ROOT / "src" / "cgame_mp" / "cg_main_mp.cpp"


def test_session_runs_source_roots_at_original_tick() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    assert "kServerTickMs = 50" in header
    assert "SV_SpawnServer(mutableMapName)" in source
    assert "SV_Frame(kServerTickMs)" in source
    assert "CL_Frame(0)" in source
    assert "SV_PacketEvent" in source
    assert "CL_PacketEvent" in source
    assert "MountSourceWeapons" in source
    assert "MountSourceModelAssets" in source
    assert "MountSourceAnimations" in source
    assert "MountSourceEffects" in source
    assert "MountSourceData" in source
    assert "VitaScriptServicesInstallRawFile" in source
    assert "RehydrateSourceStringTables" in source
    assert "Cbuf_Init();" in source
    assert "Cmd_Init();" in source
    assert "Scr_InitVariables();" in source
    assert "Scr_Init();" in source
    assert "Scr_Settings(0, 0, 0);" in source
    assert "XAnimInit();" in source
    assert "DObjInit();" in source
    assert "Netchan_Init(1);" in source
    assert "CL_InitOnceForAllClients();" in source
    assert source.index("Cbuf_Init();") < source.index("SV_Init();")
    assert source.index("Cmd_Init();") < source.index("SV_Init();")
    assert source.index("Scr_InitVariables();") < source.index("SV_Init();")
    assert source.index("Scr_Init();") < source.index("SV_Init();")
    assert source.index("XAnimInit();") < source.index("SV_Init();")
    assert source.index("DObjInit();") < source.index("SV_Init();")
    assert source.index("Netchan_Init(1);") < source.index("SV_Init();")
    assert source.index("SV_Init();") < source.index("CL_InitOnceForAllClients();")
    assert source.index("CL_InitOnceForAllClients();") < source.index("CL_Init(0);")
    assert source.index("Cmd_Init();") < source.index("CL_Init(0);")
    assert source.index("DObjShutdown();") < source.index("SL_Shutdown();")
    assert source.index("XAnimShutdown();") < source.index("SL_Shutdown();")
    assert source.index("Scr_Shutdown();") < source.index("SL_Shutdown();")
    assert "source VITP v5 weapons are not mounted" in source
    assert "source VITX v5 model closure is not mounted" in source
    assert "source VITQ v3 animation closure is not mounted" in source
    assert "source VFXA v3/VIFX v1 effect closure is not mounted" in source
    assert "source VITR/VITT data closure is not mounted" in source
    assert "clientConnectionState" in header
    assert "serverClientState" in header
    assert "connectPacketCount" in header
    assert "serverMessageSequence" in header


def test_source_hunk_uses_bounded_page_allocator() -> None:
    services = SERVICES.read_text(encoding="utf-8")
    # Allocation, zeroing, marks, failure and release behavior are executed
    # by the portable hunk test. This check covers the engine integration;
    # a fixed arena constant cannot establish total device residency.
    assert "VitaHunkArena g_sourceHunk" in services
    assert "kVitaHunkCapacity = kisak::vita::VitaHunkArena::kCapacity" in services
    assert "g_sourceHunk.Allocate(size, static_cast<size_t>(alignment))" in services
    assert "g_sourceHunk.Rewind(static_cast<size_t>(mark))" in services
    assert "g_sourceHunk.Clear();" in services
    assert "Vita source hunk allocation failed" in services


def test_weapon_memory_uses_source_ownership_before_hunk_release() -> None:
    services = SERVICES.read_text(encoding="utf-8")
    common = (ROOT / "src/qcommon/common.cpp").read_text(encoding="utf-8")
    assert '#include <qcommon/com_weapon_memory.inc>' in services
    assert '#include "com_weapon_memory.inc"' in common
    assert "void __cdecl Com_FreeWeaponInfoMemory(int) {}" not in services
    assert "void __cdecl Com_SetWeaponInfoMemory(int) {}" not in services
    shutdown = SOURCE.read_text(encoding="utf-8").split("void VitaSourceSession::Shutdown()", 1)[1]
    # Failed SV startup can bypass G_ShutdownGame, so release its remaining
    # owner before tearing down the services BG_ShutdownWeaponDefFiles uses.
    release = shutdown.index("VitaScriptServicesReleaseWeaponInfo();")
    for teardown in ("Dvar_Shutdown();", "SL_Shutdown();", "VitaScriptServicesReleaseHunk();"):
        assert release < shutdown.index(teardown)
    game = (ROOT / "src/game_mp/g_main_mp.cpp").read_text(encoding="utf-8")
    body = game.split("void __cdecl G_ShutdownGame", 1)[1]
    assert body.index("Com_FreeWeaponInfoMemory(1)") < body.index("Hunk_ClearToMarkLow(0)")


def test_session_is_one_client_offline_ffa() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert 'Dvar_SetStringByName("g_gametype", const_cast<char*>("dm"))' in source
    assert 'Dvar_SetIntByName("sv_maxclients", 1)' in source
    assert 'Dvar_SetIntByName("sv_fps", 20)' in source
    assert 'Dvar_SetBoolByName("sv_pure", false)' in source
    assert 'Dvar_SetBoolByName("onlinegame", false)' in source


def test_vita_client_parses_real_local_gamestate_without_iwd_downloads() -> None:
    parser = PARSER.read_text(encoding="utf-8")
    body = parser.split("void __cdecl CL_ParseServerMessage", 1)[1].split(
        "clSnapshot_t newSnap", 1
    )[0]
    vita_body = body.split("#ifdef KISAK_VITA_SLICE", 1)[1].split("#else", 1)[0]
    assert "case svc_gamestate:" in vita_body
    assert "CL_ParseGamestate(localClientNum, &msgCompressed);" in vita_body
    assert "case svc_serverCommand:" in vita_body
    assert "CL_ParseCommandString(localClientNum, &msgCompressed);" in vita_body
    assert "case svc_snapshot:" in vita_body
    assert "case svc_download:" in vita_body
    assert "unsupported download" in vita_body
    gamestate = parser.split("void __cdecl CL_ParseGamestate", 1)[1].split(
        "void __cdecl CL_ParseCommandString", 1
    )[0]
    assert "CL_SystemInfoChanged(localClientNum);" in gamestate
    assert "CL_InitDownloads(localClientNum);" in gamestate


def test_vita_download_completion_enters_real_cgame_without_pure_iwd_path() -> None:
    client = CLIENT.read_text(encoding="utf-8")
    body = client.split("void __cdecl CL_DownloadsComplete", 1)[1].split(
        "uint8_t msgBuffer", 1
    )[0]
    vita_body = body.split("#if defined(KISAK_VITA_SLICE)", 1)[1].split(
        "#else", 1
    )[0]
    assert "CL_InitCGame(localClientNum);" in vita_body
    assert "cls.hunkUsersStarted = 1;" in vita_body
    assert vita_body.count("CL_WritePacket(localClientNum);") == 3
    assert "CL_SendPureChecksums(localClientNum);" not in vita_body


def test_vita_client_frame_owns_handshake_and_usercmd_send() -> None:
    client = CLIENT.read_text(encoding="utf-8")
    input_source = INPUT.read_text(encoding="utf-8")
    frame_body = client.split("void __cdecl CL_Frame(int localClientNum)", 1)[1]
    vita_frame = frame_body.split("#if defined(KISAK_VITA_SLICE)", 1)[1].split("#else", 1)[0]
    assert "CL_CheckForResend(localClientNum)" in vita_frame
    # Active cgame creates the usercmd once, before prediction. CL_Frame
    # retains connection/snapshot timing and pre-active command sending.
    assert "CL_Input(localClientNum)" not in vita_frame
    assert "CL_SendCmd(localClientNum)" in vita_frame
    session = SOURCE.read_text(encoding="utf-8")
    frame = session.split("bool VitaSourceSession::Frame", 1)[1].split(
        "void VitaSourceSession::UpdatePresentation", 1)[0]
    assert frame.count("CG_DrawActiveFrame(0,") == 1
    assert frame.index("CG_DrawActiveFrame(0,") < frame.index("CL_SendCmd(0);")
    cgame_view = (ROOT / "src/cgame_mp/cg_view_mp.cpp").read_text(encoding="utf-8")
    assert "CL_Input(localClientNum);" in cgame_view
    assert "VitaInput_ReadSourceSample" in input_source
    assert "VitaInput_Poll" not in input_source.split(
        "usercmd_s *__cdecl CL_CreateCmd", 1)[1].split(
            "void __cdecl CL_AdjustAngles", 1)[0]
    assert "CL_FinishMove(localClientNum, &cmd)" in input_source


def test_vita_client_global_root_keeps_source_dvars_without_desktop_ui() -> None:
    client = CLIENT.read_text(encoding="utf-8")
    body = client.split("void __cdecl CL_InitOnceForAllClients()", 1)[1].split(
        "void __cdecl CL_Disconnect_f()", 1
    )[0]
    assert "CL_InitInput();" in body
    assert 'cl_timeout = Dvar_RegisterFloat("cl_timeout", 40.0' in body
    assert 'cl_maxpackets = Dvar_RegisterInt(' in body
    assert "CG_RegisterDvars();" in body
    assert "#if !defined(KISAK_VITA_SLICE)" in body
    assert "commands remain registered by CL_InitInput" in body
    cgame = CGAME.read_text(encoding="utf-8")
    dynamic_entity_dvars = cgame.split("CG_ViewRegisterDvars();", 1)[1].split(
        "CG_OffhandRegisterDvars();", 1
    )[0]
    # Hitscan and melee events reach the original dynamic-entity path even
    # when the map has no active dynamic props. Its dvars must always exist.
    assert "DynEntCl_RegisterDvars();" in dynamic_entity_dvars
    assert "#if" not in dynamic_entity_dvars


if __name__ == "__main__":
    test_session_runs_source_roots_at_original_tick()
    test_source_hunk_uses_bounded_page_allocator()
    test_weapon_memory_uses_source_ownership_before_hunk_release()
    test_session_is_one_client_offline_ffa()
    test_vita_client_parses_real_local_gamestate_without_iwd_downloads()
    test_vita_download_completion_enters_real_cgame_without_pure_iwd_path()
    test_vita_client_frame_owns_handshake_and_usercmd_send()
    test_vita_client_global_root_keeps_source_dvars_without_desktop_ui()
    print("vita source session tests: ok")
