#!/usr/bin/env python3
"""Audit the final Vita ELF for source-owned offline MP lifecycle symbols.

The linker uses section garbage collection, so a prototype method present in
the final executable is reachable from the shipping composition root rather
than merely sitting unused in an archive. Report mode always writes evidence;
enforce mode is the acceptance gate and fails until every required source loop
is present and every gameplay-owning Vita prototype method is absent.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from typing import Iterable


REQUIRED_GROUPS = {
    "server_frame": ("SV_Frame(int)",),
    "client_frame": ("CL_Frame(int)",),
    "game_frame": ("G_RunFrame(int)",),
    "combat": ("G_Damage(",),
    "missile": ("G_FireGrenade(", "G_FireRocket("),
    "weapon_state": ("PM_Weapon(",),
    "client_presentation": ("CG_DrawActiveFrame(",),
    "client_snapshots": ("CG_ProcessSnapshots(",),
    "client_prediction": ("CG_PredictPlayerState(",),
    "client_events": ("CG_EntityEvent(",),
    "client_viewmodel": ("CG_AddViewWeapon(",),
    "client_effects": ("FX_UpdateRemaining(",),
    "sound_update": ("SND_Update()",),
    "sound_alias_selection": ("Com_PickSoundAliasFromList(",),
    "sound_playback": ("SND_PlaySoundAlias(",),
    "spawn_placement": ("GScr_PlaceSpawnPoint(",),
    "shellshock_precache": ("Scr_PrecacheShellShock()", "BG_LoadShellShockDvars("),
    # EventNeedsProcessed is inlined at -O3. Audit its externally reached
    # source event roots instead of forcing a diagnostic-only out-of-line copy.
    "client_dynamic_entities": ("DynEntCl_RegisterDvars()", "DynEntCl_EntityImpactEvent(",
                                "DynEntCl_MeleeEvent(", "DynEntCl_ExplosionEvent("),
    "client_text": ("R_AddCmdDrawText(", "DrawText2D("),
    "client_teardown": ("CL_ShutdownCGame(", "CG_Shutdown("),
    "client_model_pose": ("CG_DObjCalcPose(",),
    "client_model_skin": ("R_SkinXSurfaceSkinned(", "R_MultiplySkelMat("),
}

SIMULATION_GROUPS = tuple(REQUIRED_GROUPS)[:6]
SOURCE_PROVIDERS = {
    "SV_Frame(int)": "sv_main_mp.cpp.obj",
    "CL_Frame(int)": "cl_main_mp.cpp.obj",
    "G_RunFrame(int)": "g_main_mp.cpp.obj",
    "G_Damage(": "g_combat_mp.cpp.obj",
    "G_FireGrenade(": "g_missile.cpp.obj",
    "G_FireRocket(": "g_missile.cpp.obj",
    "PM_Weapon(": "bg_weapons.cpp.obj",
    "CG_DrawActiveFrame(": "cg_view_mp.cpp.obj",
    "CG_ProcessSnapshots(": "cg_snapshot_mp.cpp.obj",
    "CG_PredictPlayerState(": "cg_predict_mp.cpp.obj",
    "CG_EntityEvent(": "cg_event.cpp.obj",
    "CG_AddViewWeapon(": "cg_weapons.cpp.obj",
    "FX_UpdateRemaining(": "fx_update.cpp.obj",
    "SND_Update()": "snd.cpp.obj",
    "Com_PickSoundAliasFromList(": "com_sndalias.cpp.obj",
    "SND_PlaySoundAlias(": "snd.cpp.obj",
    "GScr_PlaceSpawnPoint(": "g_scr_main_mp.cpp.obj",
    "Scr_PrecacheShellShock()": "g_scr_main_mp.cpp.obj",
    "BG_LoadShellShockDvars(": "bg_misc.cpp.obj",
    "DynEntCl_RegisterDvars()": "DynEntity_client.cpp.obj",
    "DynEntCl_EntityImpactEvent(": "DynEntity_client.cpp.obj",
    "DynEntCl_MeleeEvent(": "DynEntity_client.cpp.obj",
    "DynEntCl_ExplosionEvent(": "DynEntity_client.cpp.obj",
    "R_AddCmdDrawText(": "r_textcmds.cpp.obj",
    "DrawText2D(": "rb_text.cpp.obj",
    "CL_ShutdownCGame(": "cl_cgame_mp.cpp.obj",
    "CG_Shutdown(": "cg_main_mp.cpp.obj",
    "CG_DObjCalcPose(": "cg_pose_mp.cpp.obj",
    "R_SkinXSurfaceSkinned(": "r_model_skin_cpu.cpp.obj",
    "R_MultiplySkelMat(": "r_model_skin_cpu.cpp.obj",
}

FORBIDDEN_GROUPS = {
    "prototype_server_tick": ("ShipmentDmServer::Tick(",),
    "prototype_player_lifecycle": (
        "ShipmentDmServer::SimulatePlayer(",
        "ShipmentDmServer::RespawnPlayer(",
    ),
    "prototype_combat": (
        "ShipmentDmServer::HandleWeapon(",
        "ShipmentDmServer::ResolveHitscan(",
    ),
    "prototype_grenades": ("ShipmentDmServer::SimulateGrenades(",),
    "prototype_snapshots": (
        "ShipmentDmServer::PublishSnapshot(",
        "ShipmentDmClient::Frame(",
        "VitaMpClientBridge_StageSnapshot(",
    ),
    "prototype_weapondef_storage": ("g_vitaWeaponDefStorage",),
}


def map_providers(link_map: str) -> dict[str, str]:
    """Read allocated text definitions, excluding discarded sections/references."""
    providers = {}
    allocated = False
    provider = ""
    for line in link_map.splitlines():
        if line.startswith("Linker script and memory map"):
            allocated = True
        if line.startswith("Cross Reference Table"):
            break
        if not allocated:
            continue
        obj = re.search(r"(\S+\.a\([^()]+\.obj\)|\S+\.obj)\s*$", line)
        if obj:
            provider = obj.group(1)
        # GNU ld emits demangled symbol definitions on their own address line.
        symbol = re.match(r"^\s+0x[0-9a-f]+\s+([^=]+)$", line)
        if symbol and provider:
            value = symbol.group(1).strip()
            for needle in SOURCE_PROVIDERS:
                if value.startswith(needle):
                    providers[needle] = provider
    return providers


def audit(symbol_text: str, link_map: str = "") -> dict[str, object]:
    required: dict[str, dict[str, object]] = {}
    forbidden: dict[str, list[str]] = {}
    providers = map_providers(link_map)
    for name, needles in REQUIRED_GROUPS.items():
        matches = [needle for needle in needles if needle in symbol_text]
        required[name] = {
            "present": len(matches) == len(needles),
            "requiredSymbols": list(needles),
            "matchedSymbols": matches,
            "providers": {needle: providers.get(needle) for needle in needles},
            "providersVerified": all(providers.get(needle, "").endswith(
                "(" + SOURCE_PROVIDERS[needle] + ")") for needle in needles),
        }
    for name, needles in FORBIDDEN_GROUPS.items():
        matches = [needle for needle in needles if needle in symbol_text]
        if matches:
            forbidden[name] = matches
    missing = [name for name, result in required.items()
               if not bool(result["present"])]
    bad_providers = [name for name, result in required.items() if not result["providersVerified"]]
    simulation_passed = not forbidden and not set(SIMULATION_GROUPS).intersection(missing + bad_providers)
    passed = not missing and not forbidden and not bad_providers
    return {
        "schemaVersion": 2,
        "gate": "vita-source-owned-offline-mp",
        "passed": passed,
        "simulationLinkPassed": simulation_passed,
        "symbolPresencePassed": not missing and not forbidden,
        "providerEvidenceAvailable": bool(link_map),
        "unverifiedProviderGroups": bad_providers,
        "behaviorQualified": False,
        "missingRequiredGroups": missing,
        "forbiddenPrototypeGroups": forbidden,
        "requiredGroups": required,
        "explanation": (
            "required source definitions and implementation providers are linked; behavioral qualification is separate"
            if passed else
            "development ELF has missing or unverified source implementations, or retains prototype gameplay"
        ),
    }


def run_nm(nm: Path, binary: Path) -> str:
    completed = subprocess.run(
        [str(nm), "-C", "--defined-only", str(binary)],
        check=False, capture_output=True, text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"nm failed with {completed.returncode}: {completed.stderr.strip()}"
        )
    return completed.stdout


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--nm", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--link-map", type=Path)
    parser.add_argument("--scope", choices=("complete", "simulation"), default="complete",
                        help="Simulation permits incomplete client presentation for development builds only")
    parser.add_argument("--mode", choices=("report", "enforce"),
                        default="report")
    args = parser.parse_args(list(argv) if argv is not None else None)
    try:
        if not args.binary.is_file():
            raise RuntimeError(f"Vita ELF does not exist: {args.binary}")
        if not args.nm.is_file():
            raise RuntimeError(f"nm does not exist: {args.nm}")
        result = audit(run_nm(args.nm, args.binary), args.link_map.read_text() if args.link_map else "")
        if args.link_map:
            result["linkMapSha256"] = sha256_file(args.link_map)
        result["binary"] = args.binary.name
        result["binarySha256"] = sha256_file(args.binary)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n",
                               encoding="utf-8")
        state = "PASS" if result["passed"] else "FAIL"
        print(f"Vita source ownership: {state} ({args.output})")
        if not result["passed"]:
            print("missing:", ", ".join(result["missingRequiredGroups"]) or "none")
            print("forbidden:", ", ".join(result["forbiddenPrototypeGroups"]) or "none")
        selected_passed = result["simulationLinkPassed"] if args.scope == "simulation" else result["passed"]
        return 0 if args.mode == "report" or selected_passed else 2
    except (OSError, RuntimeError) as error:
        print(f"vita-source-ownership: {error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
