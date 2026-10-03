#!/usr/bin/env python3
"""Regression checks for the source-controlled COD4 Vita frontend route."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
FRONTEND = ROOT / "src" / "vita" / "vita_frontend.cpp"


def _route(source: str, name: str) -> list[tuple[str, bool, str]]:
    match = re.search(
        rf"constexpr VitaFrontendRouteItem {name}\[\] = \{{(.*?)\n\}};",
        source,
        re.DOTALL,
    )
    assert match, f"missing route array: {name}"
    entries = re.findall(
        r'\{"([A-Z][A-Z0-9_]+)",\s*"[^"]+",\s*'
        r'(true|false|kisak::vita::VitaMapIsEnabled\(\d+\)),\s*'
        r"VitaFrontendAction::([A-Za-z]+)\}",
        match.group(1),
    )
    result: list[tuple[str, bool, str]] = []
    for key, enabled, action in entries:
        if enabled == "true":
            is_enabled = True
        elif enabled == "false":
            is_enabled = False
        else:
            map_index = int(re.search(r"\d+", enabled).group(0))
            is_enabled = map_index in {0, 2}
        result.append((key, is_enabled, action))
    return result


def main() -> int:
    source = FRONTEND.read_text(encoding="utf-8")
    main_route = _route(source, "kMainRoute")
    multiplayer_route = _route(source, "kMultiplayerRoute")
    game_mode_route = _route(source, "kGameModeRoute")
    map_route = _route(source, "kMapRoute")
    options_route = _route(source, "kOptionsRoute")
    controls_route = _route(source, "kControlsRoute")
    controls_look_route = _route(source, "kControlsLookRoute")
    pause_route = _route(source, "kPauseRoute")

    assert [item[0] for item in main_route] == [
        "MENU_SINGLE_PLAYER",
        "MENU_MULTIPLAYER",
        "MENU_OPTIONS",
        "MENU_QUIT",
    ]
    assert [item[0] for item in multiplayer_route] == [
        "MENU_PLAYER_MATCH",
        "MPUI_CREATE_A_CLASS_CAP",
        "MPUI_BARRACKS_CAP",
        "MENU_CHALLENGES_CAP",
        "MENU_MULTIPLAYER_OPTIONS",
        "MENU_BACK",
    ]
    assert multiplayer_route[1] == (
        "MPUI_CREATE_A_CLASS_CAP", False, "Disabled"
    )
    assert game_mode_route == [
        ("MPUI_DEATHMATCH", True, "SelectDeathmatch"),
        ("MPUI_TEAM_DEATHMATCH", False, "Disabled"),
        ("MPUI_DOMINATION", False, "Disabled"),
        ("MPUI_HEADQUARTERS", False, "Disabled"),
        ("MPUI_SEARCH_AND_DESTROY", False, "Disabled"),
        ("MPUI_SABOTAGE", False, "Disabled"),
        ("MENU_BACK", True, "Back"),
    ]
    assert [item[0] for item in options_route] == [
        "MENU_CONTROLS",
        "MENU_GRAPHICS",
        "MENU_TEXTURE_SETTINGS",
        "MENU_SOUND",
        "MENU_VOICECHAT",
        "MENU_GAME_OPTIONS",
        "MENU_MULTIPLAYER_OPTIONS",
        "MENU_RESET_SYSTEM_DEFAULTS",
        "MENU_BACK",
    ]
    assert options_route[0] == ("MENU_CONTROLS", True, "OpenControls")
    assert options_route[1] == ("MENU_GRAPHICS", True, "OpenGraphics")
    assert all(not enabled for _, enabled, _ in options_route[2:-1])
    assert controls_route == [
        ("MENU_LOOK", True, "OpenControlsLook"),
        ("MENU_MOVE", False, "Disabled"),
        ("MENU_COMBAT", False, "Disabled"),
        ("MENU_INTERACT", False, "Disabled"),
        ("MENU_MULTIPLAYER_CONTROLS", False, "Disabled"),
        ("MENU_SET_DEFAULT_CONTROLS", True, "ResetControls"),
        ("MENU_BACK", True, "Back"),
    ]
    assert controls_look_route == [
        ("MENU_INVERT_MOUSE", True, "ToggleInvertY"),
        ("MENU_LEAN_LEFT", False, "Disabled"),
        ("MENU_LEAN_RIGHT", False, "Disabled"),
        ("MENU_LOOK_UP", False, "Disabled"),
        ("MENU_LOOK_DOWN", False, "Disabled"),
        ("MENU_TURN_LEFT", False, "Disabled"),
        ("MENU_TURN_RIGHT", False, "Disabled"),
        ("MENU_MOUSE_LOOK", False, "Disabled"),
        ("MENU_CENTER_VIEW", False, "Disabled"),
        ("MENU_FREE_LOOK", False, "Disabled"),
        ("MENU_SMOOTH_MOUSE", False, "Disabled"),
        ("MENU_MOUSE_SENSITIVITY", True, "AdjustSensitivity"),
        ("MENU_BACK", True, "Back"),
    ]
    assert [item[0] for item in map_route] == [
        "MPUI_SHIPMENT",
        "MPUI_CRASH",
        "MPUI_CROSSFIRE",
        "MPUI_BOG",
        "MPUI_COUNTDOWN",
        "MPUI_FARM",
        "MPUI_PIPELINE",
        "MPUI_SHOWDOWN",
        "MPUI_STRIKE",
        "MPUI_VACANT",
        "MPUI_CARGOSHIP",
        "MPUI_BACKLOT",
        "MPUI_BLOC",
        "MPUI_BROADCAST",
        "MPUI_CARENTAN",
        "MPUI_CITYSTREETS",
        "MPUI_CONVOY",
        "MPUI_CREEK",
        "MPUI_KILLHOUSE",
        "MPUI_OVERGROWN",
        "MENU_BACK",
    ]
    assert map_route[0] == ("MPUI_SHIPMENT", True, "StartShipment")
    # The certified vertical slice exposes only Shipment and Crossfire.  The
    # complete retail catalog remains visible but cannot receive focus.
    enabled_map_indices = {0, 2}
    assert all(
        enabled == (index in enabled_map_indices)
        for index, (_, enabled, _) in enumerate(map_route[:-1])
    )
    assert "VitaMapIsEnabled" in source
    assert "SetAvailableMapMask" in source
    assert "kVitaCertifiedMapMask" in source
    assert map_route[-1] == ("MENU_BACK", True, "Back")
    assert options_route[-1] == ("MENU_BACK", True, "Back")
    assert pause_route == [
        ("MENU_RESUME", True, "ResumeGame"),
        ("MENU_OPTIONS", True, "OpenOptions"),
        ("MENU_QUIT", True, "ExitToMain"),
    ]

    all_items = (main_route + multiplayer_route + game_mode_route + map_route + options_route
                 + controls_route + controls_look_route + pause_route)
    keys = [key for key, _, _ in all_items]
    assert len(keys) == 70
    assert len(set(keys)) == 62  # MENU_BACK and shared PC labels are reused.
    assert main_route[1][2] == "OpenMultiplayer"
    assert multiplayer_route[0][2] == "OpenGameModeSelect"
    assert game_mode_route[0][2] == "SelectDeathmatch"
    assert all(not enabled for _, enabled, _ in game_mode_route[1:-1])
    assert 'Title("MENU_CHOOSE_GAME_MODE_CAP", "CHOOSE GAME MODE")' in source
    assert "canStartMap && IsDeathmatch()" in source
    assert multiplayer_route[4][2] == "OpenOptions"
    assert options_route[1][2] == "OpenGraphics"
    disabled_case = re.search(
        r"case VitaFrontendAction::Disabled:.*?return false;",
        source,
        re.DOTALL,
    )
    assert disabled_case, "disabled rows must not report a handled accept"
    assert "FindRuntimeMenuItem" in source
    assert "ParseRetailMenuAction" in source
    assert 'command.verb == "open"' in source
    assert 'command.verb == "close" && command.argument == "self"' in source
    assert 'command.verb == "exec" && command.argument == "wait"' in source
    assert 'command.verb == "cmd"' in source
    assert 'command == "uiscript"' in source
    assert "bool unsafe = false" in source
    assert "ExecuteRetailMenuAction" in source
    assert "VitaFrontendScreen::CustomClasses" in source
    assert "VitaFrontendScreen::ClassEditor" in source
    assert "VitaFrontendScreen::ClassPicker" in source
    assert "mp/statstable.csv" in source
    assert "Lookup" in source
    assert "ResetCustomClassStats" in source
    assert "classSelectionTargetStat_" in source
    assert "SetStringTableBundle" in (
        ROOT / "src" / "vita" / "vita_frontend.h"
    ).read_text(encoding="utf-8")
    assert "VitaStringTableBundle" in (
        ROOT / "src" / "vita" / "vita_frontend.h"
    ).read_text(encoding="utf-8")
    assert 'if (lastOpen == "main_controls")' in source
    assert "screen_ = VitaFrontendScreen::Controls;" in source
    assert "ui_shared routes slider/listbox changes through LEFTARROW/RIGHTARROW" in source
    assert "items[selection_].action == VitaFrontendAction::AdjustSensitivity" in source
    assert "row only focuses it" in source
    assert "commands such as vote/cmd" in source
    assert "VitaMenuPlacement::ApplyX" in source
    assert "VitaMenuPlacement::ApplyY" in source
    assert "VitaMenuPlacement::NdcX" in source
    assert "VitaMenuPlacement::NdcY" in source
    assert "std::clamp(menuItem->textScale" in source
    assert "optionsRouteWithControls" in source
    assert "backRouteItem" in source
    assert "top-edge helper" in source
    assert "static_cast<float>(index) * 24.0f" in source
    assert "ResolveRetailFont" in source
    assert "scale * 48.0f" in source
    assert "font->pixelHeight" in source
    assert "R_NormalizedTextScale" in source
    assert "ITEM_TEXTSTYLE_SHADOWEDMORE" in source
    assert "textStyle == 6" in source
    assert "glyphAnchor - static_cast<float>(glyph.y0)" in source
    assert "DrawText(gfx, title, titleX, titleY, 0.5833f" in source
    assert "void DrawFooterCross" in source
    assert "void DrawFooterCircle" in source
    assert 'DrawText(gfx, "SELECT"' in source
    assert 'DrawText(gfx, "BACK"' in source
    assert 'DrawText(gfx, "START"' not in source
    assert 'DrawText(gfx, "EXIT"' not in source
    assert "const float footerText[]" in source
    assert "Source menuDef items all paint their window" in source
    assert "textTop = top - 0.078f" in source
    assert "itemTextStyle = menuItem->textStyle" in source
    assert "zero-area textured quad" in source
    assert "glyph.pixelWidth == 0 || glyph.pixelHeight == 0" in source
    assert '"MENU_MOUSE_SENSITIVITY"' in source
    assert '"MENU_INVERT_MOUSE"' in source
    assert "DrawLookValue" in source
    assert '"PLATFORM_YES"' in source
    assert '"MENU_MEDIUM"' in source
    assert "0.40f" in source
    assert "Script-driven main-menu routes" in source
    assert "SetMapThumbnail" in (ROOT / "src" / "vita" / "vita_frontend.h").read_text(encoding="utf-8")
    assert "mapThumbnailTexture_" in source
    assert 'DrawTexturedRect(gfx, drawMapThumbnailTexture' in source
    bootstrap_source = (ROOT / "src" / "vita" / "vita_bootstrap.cpp").read_text(encoding="utf-8")
    assert "InitializeDefaultVitaProfile" in bootstrap_source
    assert 'ux0:data/CODVIT001/profiles/active.txt' in bootstrap_source
    assert 'ux0:data/CODVIT001/players/profiles/User/config_mp.cfg' in bootstrap_source
    assert 'seta name \\"User\\"' in bootstrap_source
    assert 'static constexpr char kActiveProfile[] = "User\\n";' in bootstrap_source
    assert 'static constexpr char kDefaultMpConfig[] = "seta name \\"User\\"\\n";' in bootstrap_source
    assert 'profile_user_ready' in bootstrap_source
    assert "frontend.SetMapThumbnail(compassTexture.value != 0 ? compassTexture" in bootstrap_source
    assert "frontend.SetFontBundle(&shipmentFonts, uiFontTexture)" in bootstrap_source
    assert "CG_DrawPlayerWeaponIcon uses the weapon's authored ammoCounterIcon" in bootstrap_source
    assert "glyphAnchor - static_cast<float>(glyph.y0) * yScale" in bootstrap_source
    assert "glyphAnchor - static_cast<float>(glyph.y0) * pixelsPerUnit" in bootstrap_source
    mp5_stage = bootstrap_source.index(
        'StageImage(images->Find("weapon_mp5_c"));'
    )
    shipment_dressing = bootstrap_source.index(
        "static constexpr const char* kShipmentDressingPriority[]"
    )
    assert bootstrap_source.index(
        'StageImage(images->Find("weapon_m4_col"));'
    ) < mp5_stage < shipment_dressing
    assert "static constexpr float kDpadOffsets[4][2]" in bootstrap_source
    assert "static constexpr float kDpadDims[4][2]" in bootstrap_source
    assert "static constexpr float kDpadST[4][4]" in bootstrap_source
    assert 'menus, "dpad", "slot1"' in bootstrap_source
    assert 'DrawHudXpTicker(gfx' not in bootstrap_source
    assert "80.0f, 10.0f" in bootstrap_source
    assert "weaponNameWidthVirtual" in bootstrap_source
    assert "- weaponNameWidthVirtual - 28.0f" in bootstrap_source
    assert "LoadWorldPreview(database, preview," in bootstrap_source
    assert "LoadShipmentLightGrid(database, preview," in bootstrap_source
    assert "void DrawCompassTicker" in bootstrap_source
    assert "DrawHudPhysicalTextureUv" in bootstrap_source
    assert '"compass_ticker"' in bootstrap_source
    assert '"compass_ticker_background"' in bootstrap_source
    assert '"minimap_tickertape_256x16"' in bootstrap_source
    assert '"compass", "compass_ticker", "compass_ticker_background"' in bootstrap_source
    assert "float VitaStateAngleDegrees(float angle)" in bootstrap_source
    assert "angle * (360.0f / 65536.0f)" in bootstrap_source
    assert "const float compassYaw = VitaStateAngleDegrees(player.angles[1])" in bootstrap_source
    assert 'static constexpr const char* kCardinalLabels[] = {"N", "E", "S", "W"}' in bootstrap_source
    assert "minimapTickerBorder" in bootstrap_source
    assert "DrawCompassTicker(gfx" in bootstrap_source
    assert "const float tickerTop = VitaHudPlacement::ApplyY(ticker.y, 0);" in bootstrap_source
    assert "if (weaponTexture.value != 0)" in bootstrap_source
    assert "{-40.0f, -41.0f, 24.0f, 24.0f}" in bootstrap_source
    assert '"weaponinfo", "offhandFragIcon"' in bootstrap_source
    assert '"weaponinfo", "offHandFragAmmo"' in bootstrap_source
    assert "DrawHudVirtualTexture(gfx, icon, iconRect.x, iconRect.y" in bootstrap_source
    assert "DrawHudVirtualText(gfx, count, countRect.x, countRect.y" in bootstrap_source
    assert 'ResolveHudImage(&shipmentHud, "low_health_overlay", "overlay_low_health")' in bootstrap_source
    menu_bundle_header = (ROOT / "src" / "vita" / "vita_menu_bundle.h").read_text(
        encoding="utf-8"
    )
    menu_bundle_source = (ROOT / "src" / "vita" / "vita_menu_bundle.cpp").read_text(
        encoding="utf-8"
    )
    assert "const VitaMenuItem* FindItem(const char* menuName" in menu_bundle_header
    assert "return EqualIgnoreCase(menu.name, name);" in menu_bundle_source
    assert "return item.name == itemName;" in menu_bundle_source
    assert "bool IsRuntimeMenu(const std::string& name)" in menu_bundle_source
    # Source HUD-menu aliases are resolved by the bootstrap's authored lookup
    # helpers; the bundle parser is intentionally name-agnostic.
    assert 'menus->FindItem(menuName, itemName)' in bootstrap_source
    assert '"compass_old", "minimapBG"' in bootstrap_source
    assert 'FindHudSourceMenuRect(\n        menus, "dpad"' in bootstrap_source
    assert '"dpad", "slot1"' in bootstrap_source
    assert '"weaponinfo", "offhandFragIcon"' in bootstrap_source
    frontend_header = (ROOT / "src" / "vita" / "vita_frontend.h").read_text(
        encoding="utf-8"
    )
    assert "void SetFontBundle" in frontend_header
    assert "void VitaFrontend::OpenPause()" in source
    assert "frontend.OpenPause()" in (ROOT / "src" / "vita" / "vita_bootstrap.cpp").read_text(encoding="utf-8")
    assert "PAUSED" in source
    shipment_dm_header = (ROOT / "src" / "vita" / "vita_shipment_dm.h").read_text(
        encoding="utf-8"
    )
    dm_rules_header = (ROOT / "src" / "vita" / "vita_dm_rules.h").read_text(
        encoding="utf-8"
    )
    assert "enum class ShipmentDmGameMode" in shipment_dm_header
    assert "kShipmentDmTimeLimitMsec = 10 * 60 * 1000" in dm_rules_header
    assert "kShipmentDmScoreLimit = 1000" in dm_rules_header
    assert "kShipmentDmKillScore = 5" in dm_rules_header
    assert "struct ShipmentDmRules" in dm_rules_header
    assert "enum class ShipmentDmMatchPhase" in shipment_dm_header
    assert "MatchTimeLimitMsec()" in shipment_dm_header
    assert "MatchScoreLimit()" in shipment_dm_header
    assert "MatchKillScore()" in shipment_dm_header
    assert "MatchServerTime()" in shipment_dm_header
    assert "MatchPhase()" in shipment_dm_header
    assert "CopyMatchState(ShipmentDmMatchState* output) const" in shipment_dm_header
    net_header = (ROOT / "src" / "vita" / "vita_net_compat.h").read_text(
        encoding="utf-8"
    )
    net_source = (ROOT / "src" / "vita" / "vita_net_compat.cpp").read_text(
        encoding="utf-8"
    )
    dm_source = (ROOT / "src" / "vita" / "vita_shipment_dm.cpp").read_text(
        encoding="utf-8"
    )
    assert "VitaLoopback_SendMatchState" in net_header
    assert "VitaLoopback_ReceiveMatchState" in net_header
    assert "g_matchStateQueues" in net_source
    assert 'kMatchMagic = 0x314D4D44' in dm_source
    assert "struct ShipmentMatchPacket" in dm_source
    assert "static_assert(sizeof(ShipmentMatchPacket) == 32)" in dm_source
    assert "packet.killScore" in dm_source
    assert "PublishMatchState" in dm_source
    assert "client_dmm1" in bootstrap_source
    assert "matchPresentation.elapsedMsec" in bootstrap_source
    assert "matchPresentation.scoreLimit" in bootstrap_source
    assert "DeathmatchRulesLoaded" in bootstrap_source
    assert "const kisak::vita::ShipmentDmRules* dmRules" in bootstrap_source
    assert "shipmentServer.MatchTimeLimitMsec()" in bootstrap_source
    assert "shipmentServer.MatchScoreLimit()" in bootstrap_source
    assert 'TextVirtual("FREE-FOR-ALL"' in bootstrap_source
    vita_cmake = (ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    assert "KISAK_VITA_MENU_ONLY" in vita_cmake
    assert "KISAK_VITA_MENU_ONLY=1" in vita_cmake
    assert "diagnostic menu-only=1" in bootstrap_source
    assert "#if defined(KISAK_VITA_MENU_ONLY)" in bootstrap_source
    assert "VitaInput_Poll(&input, &inputConfig, &inputSample);" in bootstrap_source
    print("vita frontend route tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
