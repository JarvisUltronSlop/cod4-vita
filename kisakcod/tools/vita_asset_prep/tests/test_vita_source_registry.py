"""Static contract checks for the bounded source-facing Vita registry."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / "src" / "vita" / "vita_source_registry.h"
SOURCE = ROOT / "src" / "vita" / "vita_source_registry.cpp"
SERVICES = ROOT / "src" / "vita" / "vita_script_services.cpp"


def test_registry_is_bounded_and_arena_owned() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    assert "kMaxAssets = 8192" in header
    assert "Allocate(kMaxAssets * sizeof(Entry), alignof(Entry))" in source
    assert "cursor_ = entries_ ? kMaxAssets * sizeof(Entry) : 0" in source
    assert "kMaximumArenaBytes = 96u * 1024u * 1024u" in header
    assert "void* VitaAssetRegistry::Allocate" in source
    assert "entryCount_ = 0" in source
    assert "cursor_ = 0" in source


def test_registry_exposes_typed_source_assets() -> None:
    header = HEADER.read_text(encoding="utf-8")
    for method in (
        "RegisterAnimation", "RegisterModel", "RegisterMaterial",
        "RegisterImage", "RegisterSound", "RegisterWeapon",
        "RegisterClipMap", "RegisterComWorld", "RegisterGfxWorld",
        "RegisterRawFile", "RegisterStringTable",
    ):
        assert method in header
    assert "RehydrateSoundAlias" in header
    assert "RehydrateFont" in header
    assert "RehydrateWeapon" in header
    assert "RehydrateSourceWeapons" in header
    assert "RehydrateImage" in header
    assert "RehydrateMaterial" in header
    assert "RehydrateModel" in header
    assert "RehydrateAnimation" in header
    assert "RehydrateSourceAnimations" in header
    assert "RehydrateSourceEffects" in header
    assert "RehydrateStringTable" in header
    assert "RehydrateSourceStringTables" in header
    assert "RehydrateSourceModelClosure" in header


def test_model_closure_requires_vitx_v5_and_builds_real_source_structures() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    assert "VITX v5 bone-name closure" in source
    assert "SL_GetStringOfSize" in source
    assert "MT_TYPE_MODEL_PART" in source
    assert "sourceName.empty() ||" not in source
    assert "model->boneNames = boneNames" in source
    assert "model->surfs = surfaces" in source
    assert "material->textureTable" in source
    assert "image->texture.loadDef = loadDef" in source
    assert "reinterpret_cast<XModel" not in source
    assert "VitaSourceModelClosureReport" in header


def test_animation_closure_reconstructs_real_compressed_xanimparts() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert "VITQ v3 compressed closure" in source
    assert "MT_TYPE_ANIM_PART" in source
    assert "MT_TYPE_NOTETRACK" in source
    assert "parts->dataByte" in source
    assert "parts->randomDataShort" in source
    assert "parts->deltaPart" in source
    assert "RegisterAnimation(name, parts" in source


def test_desktop_animation_export_matches_source_names_case_insensitively() -> None:
    desktop = (ROOT / "src" / "database" / "db_registry.cpp").read_text(
        encoding="utf-8")
    assert "const bool explicitlyNamed = std::any_of" in desktop
    assert "!I_stricmp(name.c_str(), parts->name)" in desktop
    assert "const bool mantleRuntime = std::any_of" in desktop
    for name in (
        "mp_mantle_up_57", "mp_mantle_up_21", "mp_mantle_over_high",
        "mp_mantle_over_mid", "player_mantle_over_low",
    ):
        assert f'"{name}"' in desktop


def test_desktop_animation_export_follows_retail_playeranim_closure() -> None:
    desktop = (ROOT / "src" / "database" / "db_registry.cpp").read_text(
        encoding="utf-8")
    assert 'ASSET_TYPE_RAWFILE, "mp/playeranim.script"' in desktop
    assert "DB_VitaPlayerAnimScriptReferences" in desktop
    assert "DB_VitaIsScriptIdentifierCharacter" in desktop
    assert "const bool playerAnimRuntime" in desktop
    assert "!playerAnimRuntime" in desktop
    assert "missing required mp/playeranim.script closure" in desktop
    assert "sampledParts.insert(parts)" in desktop
    assert "collect.sampledParts.find(parts)" in desktop
    assert "evaluated by source XAnim" in desktop


def test_effect_closure_reconstructs_real_fx_graphs() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert "VFXA v3 source effect closure" in source
    assert "FxElemVelStateSample" in source
    assert "FxElemVisStateSample" in source
    assert "output.trailDef->scrollTimeMsec" in source
    assert "output.visuals.markArray" in source
    assert "RegisterEffect(name, effect" in source
    assert "RegisterEffect(input->name.c_str(), effect" in source
    assert "weapon_dragunov_clip -> shellejects/clip_dragunov" in source


def test_string_tables_are_rehydrated_as_real_row_major_source_assets() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert "StringTable* VitaAssetRegistry::RehydrateStringTable" in source
    assert "table->columnCount" in source
    assert "table->rowCount" in source
    assert "table->values = values" in source
    assert "RegisterStringTable(name, table" in source


def test_offline_closure_requires_both_grenade_classes() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert 'VitaFindMapIndex(mapName,&mapIndex)' in source
    assert '"frag_grenade_mp"' in source
    assert '"defaultweapon_mp"' in source
    assert '"flash_grenade_mp"' in source
    assert '"none"' in source
    assert "report.ready = report.missingCount == 0" in source


def test_source_weapon_constructor_uses_named_fields_and_strict_dependencies() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert "KISAK_VITA_WEAPON_INT_FIELDS" in source
    assert "KISAK_VITA_WEAPON_FLOAT_FIELDS" in source
    assert "resolve(kAssetModel" in source
    assert "resolve(kAssetAnimation" in source
    assert "InternScriptString(source.hideTagNames" in source
    assert "SL_GetStringOfSize(text, 0, bytes," in source
    assert "SL_RemoveRefToString(reference->value)" in source
    assert "reinterpret_cast<WeaponDef" not in source


def test_rehydrated_assets_are_not_reported_as_fastfile_defaults() -> None:
    services = SERVICES.read_text(encoding="utf-8")
    body = services.split("bool __cdecl DB_IsXAssetDefault", 1)[1]
    body = body.split("}", 1)[0]
    assert "VitaSourcePlatform_FindAsset" in body
    assert "== nullptr" in body


if __name__ == "__main__":
    test_registry_is_bounded_and_arena_owned()
    test_registry_exposes_typed_source_assets()
    test_model_closure_requires_vitx_v5_and_builds_real_source_structures()
    test_animation_closure_reconstructs_real_compressed_xanimparts()
    test_desktop_animation_export_matches_source_names_case_insensitively()
    test_desktop_animation_export_follows_retail_playeranim_closure()
    test_effect_closure_reconstructs_real_fx_graphs()
    test_string_tables_are_rehydrated_as_real_row_major_source_assets()
    test_offline_closure_requires_both_grenade_classes()
    test_source_weapon_constructor_uses_named_fields_and_strict_dependencies()
    test_rehydrated_assets_are_not_reported_as_fastfile_defaults()
    print("vita source registry tests: ok")
