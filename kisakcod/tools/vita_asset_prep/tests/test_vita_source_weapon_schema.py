"""Contract checks for the pointer-free source-owned VITP v5 schema."""

from pathlib import Path
import re

from source_runtime_profile import SOURCE_WEAPONS


ROOT = Path(__file__).resolve().parents[3]
SCHEMA = ROOT / "src" / "vita" / "vita_weapon_record.h"
EXPORTER = ROOT / "src" / "database" / "db_registry.cpp"
LOADER = ROOT / "src" / "vita" / "vita_shipment_weapons.cpp"
SESSION = ROOT / "src" / "vita" / "vita_source_session.cpp"
CAPABILITIES = ROOT / "src" / "vita" / "vita_slice_capabilities.h"


def test_v5_is_pointer_free_and_has_a_fixed_wire_abi() -> None:
    schema = SCHEMA.read_text(encoding="utf-8")
    body = schema.split("struct VitaWeaponRecordV5", 1)[1]
    body = body.split("static_assert(std::is_standard_layout", 1)[0]
    assert "kVitaWeaponVersionSource = 5" in schema
    assert "kVitaWeaponRecordV5Bytes == 15376" in schema
    assert "void*" not in body
    assert "WeaponDef*" not in body
    assert "XModel*" not in body
    assert "snd_alias_list_t*" not in body
    assert "sourceLayoutVersion" in body
    assert "locationDamageMultipliers" in body
    assert "parallelBounce" in body
    assert "perpendicularBounce" in body


def test_exporter_selects_exact_fixed_loadout_and_named_members() -> None:
    exporter = EXPORTER.read_text(encoding="utf-8")
    target = exporter.split("static bool DB_VitaTargetWeapon(const char* name)", 2)[2]
    target = target.split("static void DB_VitaCollectModel", 1)[0]
    capabilities = CAPABILITIES.read_text(encoding="utf-8")
    names = re.findall(r'"([a-z0-9_]+)"', capabilities)
    assert len(names) == len(SOURCE_WEAPONS)
    assert set(names) == set(SOURCE_WEAPONS)
    assert "kVitaSourceWeaponNames" in target
    assert "!I_stricmp(name, required)" in target
    assert '"m4_mp"' not in target
    assert "KISAK_VITA_WEAPON_INT_FIELDS" in exporter
    assert "KISAK_VITA_WEAPON_FLOAT_FIELDS" in exporter
    assert "weapon.locationDamageMultipliers" in exporter
    assert "weapon.parallelBounce" in exporter


def test_legacy_records_cannot_certify_source_session() -> None:
    loader = LOADER.read_text(encoding="utf-8")
    session = SESSION.read_text(encoding="utf-8")
    assert "wireVersion_ != kVitaWeaponVersionSource" in loader
    assert "kVitaSourceWeaponNames" in loader
    assert "Find(name)" in loader
    assert "if (!sourceWeaponsMounted_)" in session


if __name__ == "__main__":
    test_v5_is_pointer_free_and_has_a_fixed_wire_abi()
    test_exporter_selects_exact_fixed_loadout_and_named_members()
    test_legacy_records_cannot_certify_source_session()
    print("vita source weapon schema tests: ok")
