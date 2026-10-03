#!/usr/bin/env python3
"""Verify the bounded retail GSC closure used by the Vita Shipment slice."""

from __future__ import annotations

import re
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src" / "vita" / "vita_script_runtime.cpp"
HEADER = ROOT / "src" / "vita" / "vita_script_runtime.h"
VM_SOURCE = ROOT / "src" / "vita" / "vita_script_vm_runtime.cpp"
ENTITY_SOURCE = ROOT / "src" / "vita" / "vita_script_entity_runtime.cpp"
DB_SOURCE = ROOT / "src" / "database" / "db_registry.cpp"

REQUIRED = {
    "maps/mp/mp_shipment.gsc": ("main",),
    "maps/mp/mp_shipment_fx.gsc": ("main",),
    "maps/createart/mp_shipment_art.gsc": ("main",),
    "maps/mp/_load.gsc": ("main",),
    "maps/mp/gametypes/dm.gsc": ("main", "onSpawnPlayer"),
    "maps/mp/gametypes/_spawnlogic.gsc": ("init", "onPlayerConnect"),
    "maps/mp/gametypes/_callbacksetup.gsc": (
        "CodeCallback_PlayerConnect",
        "CodeCallback_PlayerDamage",
        "CodeCallback_PlayerKilled",
    ),
}


def _rawfile_records(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    magic, version, count, declared = struct.unpack_from("<4sIIQ", data, 0)
    assert magic == b"VITR" and version == 1
    offset = 20
    records: dict[str, bytes] = {}
    aggregate = 0
    for _ in range(count):
        name_bytes, payload_bytes = struct.unpack_from("<2I", data, offset)
        offset += 8
        name = data[offset : offset + name_bytes].decode("utf-8")
        offset += name_bytes
        records[name] = data[offset : offset + payload_bytes]
        offset += payload_bytes
        aggregate += payload_bytes
    assert offset == len(data) and aggregate == declared
    return records


def _kvap_rawfile_records(path: Path) -> dict[str, bytes] | None:
    """Read the script chunk without requiring a multi-hundred-MiB run home."""
    header = struct.Struct("<4sHHBBHQIQ32s")
    toc = struct.Struct("<IIQQQQIII12s")
    with path.open("rb") as stream:
        header_data = stream.read(header.size)
        if len(header_data) != header.size:
            return None
        magic, _major, _minor, endian, _flags, header_size, toc_offset, count, *_ = header.unpack(
            header_data
        )
        if magic != b"KVAP" or endian != 1 or header_size != header.size:
            return None
        stream.seek(toc_offset)
        for _ in range(count):
            record = stream.read(toc.size)
            if len(record) != toc.size:
                return None
            kind, _chunk_flags, _asset_id, offset, stored, _raw, _alignment, codec, _crc, _reserved = toc.unpack(
                record
            )
            if kind != 10:
                continue
            stream.seek(offset)
            payload = stream.read(stored)
            if codec == 1:
                payload = zlib.decompress(payload)
            return _rawfile_records_from_bytes(payload)
    return None


def _rawfile_records_from_bytes(data: bytes) -> dict[str, bytes]:
    magic, version, count, declared = struct.unpack_from("<4sIIQ", data, 0)
    assert magic == b"VITR" and version == 1
    offset = 20
    records: dict[str, bytes] = {}
    aggregate = 0
    for _ in range(count):
        name_bytes, payload_bytes = struct.unpack_from("<2I", data, offset)
        offset += 8
        name = data[offset : offset + name_bytes].decode("utf-8")
        offset += name_bytes
        records[name] = data[offset : offset + payload_bytes]
        offset += payload_bytes
        aggregate += payload_bytes
    assert offset == len(data) and aggregate == declared
    return records


def _strip_gsc_comments_and_strings(text: str) -> str:
    out: list[str] = []
    i = 0
    state = "code"
    while i < len(text):
        c = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state, i = "line", i + 2
                out.extend("  ")
                continue
            if c == "/" and nxt == "*":
                state, i = "block", i + 2
                out.extend("  ")
                continue
            if c == '"':
                state, i = "string", i + 1
                out.append(" ")
                continue
            out.append(c)
            i += 1
        elif state == "line":
            out.append("\n" if c in "\r\n" else " ")
            state = "code" if c in "\r\n" else state
            i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                out.extend("  ")
                state, i = "code", i + 2
            else:
                out.append("\n" if c in "\r\n" else " ")
                i += 1
        else:
            if c == "\\":
                out.extend("  ")
                i += 2
            else:
                out.append(" ")
                state = "code" if c == '"' else state
                i += 1
    return "".join(out)


def _has_definition(text: str, name: str) -> bool:
    clean = _strip_gsc_comments_and_strings(text)
    pattern = re.compile(rf"(?<![A-Za-z0-9_]){re.escape(name)}\s*\([^)]*\)\s*\{{")
    return pattern.search(clean) is not None


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    dm_rules_header = (ROOT / "src" / "vita" / "vita_dm_rules.h").read_text(
        encoding="utf-8"
    )
    vm_source = VM_SOURCE.read_text(encoding="utf-8")
    script_main = (ROOT / "src" / "script" / "scr_main.cpp").read_text(encoding="utf-8")
    entity_source = ENTITY_SOURCE.read_text(encoding="utf-8")
    for script_name in REQUIRED:
        source_name = script_name.removesuffix(".gsc")
        assert source_name in source, f"runtime must name retail script: {script_name}"
    assert "StartShipmentMatch" in source
    assert "NotifyShipmentEvent" in source
    assert "VitaScriptVm_HasCallback" in source
    # Shipment's atmosphere must remain source-authored: parse the retail
    # create-art setExpFog call and hand its exponential density to the
    # renderer rather than introducing a platform-specific visual preset.
    assert "struct VitaScriptFogParams" in header
    assert "ParseRetailSetExpFog" in source
    # The Deathmatch server must consume the authored dm.gsc rule registration
    # instead of duplicating the retail defaults in a platform-only preset.
    assert "ParseRetailDeathmatchRules" in source
    assert "ParseNamedScoreInfo" in source
    assert "StripGscCommentsPreserveStrings" in source
    assert "registerTimeLimitDvar" in source
    assert "registerScoreLimitDvar" in source
    assert "registerScoreInfo" in source
    assert "DeathmatchRulesLoaded" in header
    assert "dmRulesLoaded_" in header
    assert "ParseDeathmatchRules" in header
    assert "shipmentClosureReady_ = shipmentClosureReady_ && dmRulesLoaded_" in source
    assert "struct ShipmentDmRules" in dm_rules_header
    assert "teamBased" in dm_rules_header
    assert 'record->name == "maps/createart/mp_shipment_art.gsc"' in source
    assert "0.69314718f / values[1]" in source
    assert "VitaScriptVm_CallbackHandle" in vm_source
    assert "VitaScriptServicesSetTraceAppId" in vm_source
    assert "script_parse_begin" in script_main
    assert "script_compile_begin" in script_main
    assert "SL_Init" in vm_source
    assert "g_stringListInitialized" in vm_source
    assert vm_source.index("SL_Init();") < vm_source.index("Scr_InitVariables();")
    for script_name in REQUIRED:
        native_name = script_name.removesuffix(".gsc")
        assert native_name in vm_source, (
            f"native VM load set must include {script_name}"
        )
    for script_name in ("codescripts/delete", "codescripts/struct"):
        assert f'{{"{script_name}"' in vm_source
    assert "scriptsLoaded" in vm_source
    assert "handle == 0" in vm_source
    assert "case 3" in vm_source
    assert "native" in source and "VM must have resolved" in source
    assert "eventType" in source
    assert "StripGscCommentsAndStrings" in source
    assert "HasFunctionDefinition" in source
    assert "lastCallback_" in source
    assert "PollCallbackEvent" in source
    assert "callbackQueue_" in source
    assert "clientNum" in source and "serverTime" in source
    assert "VitaScriptVm_CallbackHandle(eventType)" in source
    assert "VitaScriptVm_InitializeEntityRuntime" in entity_source
    assert "Scr_SetClassMap" in entity_source
    assert "g_scr_data.gametype.main" in entity_source
    assert "g_scr_data.levelscript" in entity_source
    assert '"maps/mp/mp_shipment", "main"' in entity_source
    assert 'VitaScriptVm_NativeFunctionHandle(filename, label)' in entity_source
    assert "NativeHandleSpec" in vm_source
    assert "g_nativeHandles" in vm_source
    assert "Scr_EndLoadScripts clears that compiler-side table" in (ROOT / "src" / "vita" / "vita_script_vm_runtime.h").read_text(encoding="utf-8")
    assert "g_scr_data.gametype.playerdamage" in entity_source
    assert 'VitaScriptServicesTrace("entity_handles", handleDetail)' in entity_source
    assert '"entity_handles"' in entity_source
    assert "KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME" in entity_source
    assert '"entity_runtime_deferred"' in entity_source
    assert "KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME" in (ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "G_SetConstString(&entity.classname" in entity_source
    assert "G_SetConstString(&entity.target" in entity_source
    assert "G_SetConstString(&entity.model" in entity_source
    assert "g_scr_data.delete_" in entity_source
    assert "for (uint32_t entityIndex = 0; entityIndex < g_materializedEntityCount; ++entityIndex)" in entity_source
    assert "Scr_GetEntityId(entityIndex, CLASS_NUM_ENTITY)" in entity_source
    assert "Scr_AddEntityNum is a VM-parameter helper" in entity_source
    assert "ShipmentMapEntities" in entity_source
    assert "Scr_StartupGameType returned no thread" in entity_source
    assert "CodeCallback_PlayerConnect returned no thread" in entity_source
    assert "Scr_Notify(&g_entities[0], scr_const.begin, 0)" in entity_source
    assert "g_vitaPersId" in entity_source
    assert "sess.scriptPersId" in entity_source
    assert 'SetObjectStringField(g_vitaPersId, "team", "allies")' in entity_source
    assert "sess.connected = CON_CONNECTED" in entity_source
    assert "sess.sessionState = SESS_STATE_PLAYING" in entity_source
    assert "ReleasePersistentObject" in entity_source
    assert "Scr_AllocGameVariable" in entity_source
    # The desktop MP bootstrap executes codescripts/struct::initstructs between
    # game-variable allocation and gametype startup.  Keep the Vita probe's
    # source-backed ordering visible so a future native-runtime change cannot
    # silently regress to the pre-struct initialization that produced field
    # object errors in dm.main.
    assert "before_initstructs" in entity_source
    assert "g_scr_data.initstructs" in entity_source
    assert entity_source.index("Scr_AllocGameVariable();") < entity_source.index("before_initstructs")
    assert entity_source.index("after_initstructs") < entity_source.index("before_dm_main")
    assert "InitializeNativeServerConfigstrings" in entity_source
    assert 'native_configstrings", "empty_table=2442 northyaw=0' in entity_source
    assert entity_source.index("after_native_configstrings") < entity_source.index("before_initstructs")
    assert "Scr_SetLoading(1)" in entity_source
    assert "Scr_SetLoading(0)" in entity_source
    assert 'SetLevelStringField("gameType", "dm")' in entity_source
    assert "Scr_ExecThread" in entity_source
    assert "g_gameTypeStarted" in entity_source
    assert "renderer-free probe path" in entity_source
    assert "player/world-only" in entity_source
    assert "Scr_ExecEntThreadNum" in entity_source
    assert "VitaScriptVm_SyncEntityState" in entity_source
    assert "VitaScriptVm_AdvanceTime" in entity_source
    assert "Scr_IncTime" in entity_source
    assert "g_lastAdvanceServerTime" in entity_source
    assert "event.attackerClientNum" in (ROOT / "src" / "vita" / "vita_bootstrap.cpp").read_text(encoding="utf-8")
    assert "functionHandle != VitaScriptVm_CallbackHandle(eventType)" in entity_source
    assert "Scr_PlayerDamage" in entity_source
    assert "Scr_PlayerKilled" in entity_source
    assert "WeaponInternalName" in entity_source
    assert 'return "m4_mp"' in entity_source
    assert 'return "mp5_mp"' in entity_source
    assert 'return "usp_mp"' in entity_source
    assert "WeaponMeansOfDeath" in entity_source
    assert "scr_const.mod_pistol_bullet" in entity_source
    assert "scr_const.mod_rifle_bullet" in entity_source
    assert "Scr_AddConstString(scr_const.none)" in entity_source
    entities_header = (ROOT / "src" / "vita" / "vita_shipment_entities.h").read_text(encoding="utf-8")
    bootstrap_source = (ROOT / "src" / "vita" / "vita_bootstrap.cpp").read_text(encoding="utf-8")
    assert "shipmentScripts.Fog()" in bootstrap_source
    assert 'VitaScriptServicesTrace("renderer_fog", fogDetail)' in bootstrap_source
    assert "struct ShipmentMapEntity" in entities_header
    assert "Records() const" in entities_header
    assert "int32_t count = 0" in entities_header
    assert "int32_t health = 0" in entities_header
    assert 'ReadQuoted(block, "count"' in (ROOT / "src" / "vita" / "vita_shipment_entities.cpp").read_text(encoding="utf-8")
    assert 'ReadQuoted(block, "health"' in (ROOT / "src" / "vita" / "vita_shipment_entities.cpp").read_text(encoding="utf-8")
    entities_source = (ROOT / "src" / "vita" / "vita_shipment_entities.cpp").read_text(encoding="utf-8")
    db_source = DB_SOURCE.read_text(encoding="utf-8")
    mp_script_source = (ROOT / "src" / "game_mp" / "g_scr_main_mp.cpp").read_text(
        encoding="utf-8"
    )
    assert 'ReadQuoted(block, "model"' in entities_source
    assert 'ReadQuoted(block, "script_linkname"' in entities_source
    assert 'ReadQuoted(block, "script_noteworthy"' in entities_source
    assert "VitaScriptVm_InitializeEntityRuntime(&shipmentEntities)" in bootstrap_source
    assert "DB_VitaCollectMapEntityModels" in db_source
    assert 'classname == "script_model"' in db_source
    assert "DrawStaticShipmentModels" in bootstrap_source
    assert 'VitaScriptServicesTrace("static_models_drawn"' in bootstrap_source
    assert "numParams <= 1" in mp_script_source
    assert 'dvarValue = ""' in mp_script_source
    assert "Scr_ConstructMessageString(1, NumParam - 1" in mp_script_source
    assert "The Vita package is a deliberate Shipment closure" in mp_script_source
    assert "defined(KISAK_NO_FASTFILES) || defined(KISAK_VITA_SLICE)" in mp_script_source
    assert "Shipment sidecar owns spawn placement" in mp_script_source
    assert "Shipment's Vita closure does not carry the campaign shell-shock" in mp_script_source
    assert "BG_LoadShellShockDvars" in mp_script_source
    assert "attackerClientNum" in header
    assert "direction[3]" in header
    assert "kCallbackQueueCapacity = 64" in header
    # Fire/reload are intentionally not presented as GSC callbacks: the
    # retail closure only exposes spawn, damage, and killed entry points.
    assert 'case 2: // reload_start/reload' not in source
    assert 'CodeCallback_PlayerDamage' in source
    assert 'CodeCallback_PlayerKilled' in source

    candidates = sorted(
        (ROOT.parent / "build" / "windows" / "reference" / "runtime-home").glob(
            "*/main/vita_rawfiles.bin"
        )
    )
    if candidates:
        # The reference runner may leave several map captures in this shared
        # home.  Choose the newest capture that actually contains the common
        # Shipment script closure instead of assuming the newest map dump is
        # Shipment (the map sweep can legitimately run after it).
        required_names = ("codescripts/delete.gsc", "codescripts/struct.gsc", *REQUIRED)
        records = None
        for candidate in reversed(candidates):
            candidate_records = _rawfile_records(candidate)
            if all(name in candidate_records for name in required_names):
                records = candidate_records
                break
        # A retained reference run may be for another map (for example,
        # Crossfire) and therefore omit Shipment's map-local scripts.  The
        # cleaned workspace keeps the complete script chunk in the active
        # package, so use that compact canonical source before declaring the
        # asset-side closure unavailable.
        if records is None:
            package_candidates = (
                ROOT.parent
                / "assets"
                / "packages"
                / "mp-map-fidelity-retailclosure-color-tail-model320-venv3.kvap",
                ROOT / "assets" / "packages" / "shipment-db-reference.kvap",
            )
            for package in package_candidates:
                if not package.is_file():
                    continue
                package_records = _kvap_rawfile_records(package)
                if package_records is not None and all(
                    name in package_records for name in required_names
                ):
                    records = package_records
                    break
        if records is not None:
            for script_name, functions in {
                "codescripts/delete.gsc": ("main",),
                "codescripts/struct.gsc": ("InitStructs", "CreateStruct"),
            }.items():
                payload = records.get(script_name)
                assert payload is not None, f"missing native support rawfile: {script_name}"
                text = payload.decode("latin-1")
                for function in functions:
                    assert _has_definition(text, function), (
                        f"{function} must be a definition in {script_name}"
                    )
            for script_name, functions in REQUIRED.items():
                payload = records.get(script_name)
                assert payload is not None, f"missing retail rawfile: {script_name}"
                text = payload.decode("latin-1")
                for function in functions:
                    assert re.search(
                        rf"(?<![A-Za-z0-9_]){re.escape(function)}\s*\(", text
                    ), f"missing {function} in {script_name}"
                    assert _has_definition(text, function), (
                        f"{function} must be a definition, not a comment/call"
                    )
            dm_text = records["maps/mp/gametypes/dm.gsc"].decode("latin-1")
            # Keep the asset-side fixture explicit: these are the values the C++
            # source parser must recover and pass to the local DM server.
            assert re.search(r"registerTimeLimitDvar\s*\([^;]*,\s*10\s*,", dm_text)
            assert re.search(r"registerScoreLimitDvar\s*\([^;]*,\s*1000\s*,", dm_text)
            assert re.search(r"registerScoreInfo\s*\(\s*\"kill\"\s*,\s*5\s*\)", dm_text)
            assert not _has_definition(
                "// fake()\nfoo();\n/* bar() {} */", "fake"
            )
    print("vita script runtime tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
