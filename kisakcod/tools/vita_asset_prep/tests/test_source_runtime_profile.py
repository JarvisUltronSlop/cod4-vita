import importlib.util
import struct
from pathlib import Path

MODULE = Path(__file__).resolve().parents[1] / "source_runtime_profile.py"
SPEC = importlib.util.spec_from_file_location("source_runtime_profile", MODULE)
profile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profile)


def test_weapon_identity_gate_rejects_wrong_or_duplicate_definitions():
    def fixture(names):
        return struct.pack("<4sIII", b"VITP", 5, len(names), 15376) + b"".join(
            name.encode().ljust(15376, b"\0") for name in names)
    good = fixture(profile.SOURCE_WEAPONS)
    profile.validate_source_weapon_set(good)
    for damaged in (good[:-1], good + b"extra", fixture(profile.SOURCE_WEAPONS[:-1]),
                    fixture((*profile.SOURCE_WEAPONS[:-1], "unavailable_weapon")),
                    fixture((*profile.SOURCE_WEAPONS[:-1], profile.SOURCE_WEAPONS[0]))):
        try:
            profile.validate_source_weapon_set(damaged)
        except ValueError:
            pass
        else:
            raise AssertionError("incorrect weapon set accepted")


def script_fixture():
    return {
        "maps/mp/gametypes/dm.gsc": b"main() { original_dm_rules(); }\n",
        "maps/mp/gametypes/_globallogic.gsc": b"init() {\n level.onlineGame = true;\n original_spawn();\n}\n",
        "maps/mp/gametypes/_hardpoints.gsc": (
            "init() {\n" + "\n".join(f' precacheItem( "{name}" );' for name in (
                "radar_mp", "airstrike_mp", "helicopter_mp", "artillery_mp"))
            + "\n maps\\mp\\_helicopter::init();\n original_shared_state();\n}\n"
              "giveHardpointItemForStreak()\n{\n original_awards();\n}\n").encode(),
        "maps/mp/gametypes/_gameobjects.gsc": (
            'init() {\n precacheItem( "briefcase_bomb_mp" );\n'
            ' precacheItem( "briefcase_bomb_defuse_mp" );\n original_props();\n}\n').encode(),
        "maps/mp/untouched.gsc": b"unchanged source map code\x00",
    }


def table_fixture():
    stats = [[str(i), str(3000 + i), "weapon", "LABEL", name, "", "IMAGE", "DESC", "attachment", "", ""]
             for i, name in enumerate((*profile.LOADOUT, "unavailable_weapon"))]
    stats.append(["160", "3160", "specialty", "PERK", "untouched_perk", "", "", "", "", "", ""])
    return {"mp/statstable.csv": stats,
            "mp/offline_classtable.csv": [[str(i), str(i), "class", "label", "old", "1", "2"]
                                          for i in range(200, 220)],
            "mp/untouched.csv": [["keep", "all", "these"], ["source", "values", "exact"]],
            "mp/empty.csv": []}


def test_script_profile_preserves_source_dm_and_unrelated_bytes():
    before = script_fixture()
    wire = profile.build_rawfiles(before)
    output, report = profile.profile_scripts(wire)
    assert profile.profile_scripts(wire) == (output, report)
    after = profile.rawfiles(output)
    assert set(before) == set(after)
    assert after["maps/mp/gametypes/dm.gsc"] == before["maps/mp/gametypes/dm.gsc"]
    assert after["maps/mp/untouched.gsc"] == before["maps/mp/untouched.gsc"]
    assert b"original_shared_state();" in after["maps/mp/gametypes/_hardpoints.gsc"]
    assert b"original_awards();" in after["maps/mp/gametypes/_hardpoints.gsc"]
    assert b"if ( !level.vitaHardpointsEnabled )" in after["maps/mp/gametypes/_hardpoints.gsc"]
    assert b"original_props();" in after["maps/mp/gametypes/_gameobjects.gsc"]
    assert b"level.onlineGame = false;" in after["maps/mp/gametypes/_globallogic.gsc"]
    assert len(report) == 3
    assert all(row["sourceSha256"] != row["outputSha256"] for row in report)


def test_entity_profile_preserves_dm_world_and_static_instance_seam():
    world = b'{"classname" "worldspawn" "message" "quoted { braces }"}\n'
    spawn = b'{"classname" "mp_dm_spawn" "origin" "1 2 3"}\n'
    pickup = b'{"classname" "weapon_unavailable_mp" "targetname" "oldschool_pickup" "target" "pickup_trigger"}'
    trigger = b'{"classname" "trigger_radius" "targetname" "pickup_trigger"}'
    static = b'\x00{"classname" "vita_static_model" "model" "original_prop"}\n'
    source = world + spawn + pickup + trigger + static
    wire = struct.pack("<4sII", b"VITE", 1, len(source)) + source
    output, reports = profile.profile_entities(wire)
    assert output[12:] == world + spawn + static
    assert struct.unpack_from("<I", output, 8)[0] == len(output) - 12
    assert [r["fields"]["classname"] for r in reports] == ["weapon_unavailable_mp", "trigger_radius"]
    assert profile.profile_entities(wire) == (output, reports)


def test_entity_profile_rejects_corruption_and_ambiguous_pickup_targets():
    world = b'{"classname" "worldspawn"}\n'
    malformed = (b'{"classname" "worldspawn"', world + b'garbage',
                 world + b'{"classname" "script_model" "targetname" "oldschool_pickup" "target" "missing"}',
                 world + b'{"classname" "mp_dm_spawn" "targetname" "oldschool_pickup"}',
                 world + b'{"classname" "script_model" "classname" "duplicate"}',
                 world + b'{"classname"\x00"script_model"}')
    for payload in malformed:
        try:
            profile.profile_entities(struct.pack("<4sII", b"VITE", 1, len(payload)) + payload)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid entity configuration accepted")


def test_script_shape_change_cannot_silently_apply_profile():
    for mutate in (lambda records: records.pop("maps/mp/gametypes/_hardpoints.gsc"),
                   lambda records: records.update({"maps/mp/gametypes/_globallogic.gsc": b"new_init() {}"})):
        records = script_fixture()
        mutate(records)
        try:
            profile.profile_scripts(profile.build_rawfiles(records))
        except ValueError:
            pass
        else:
            raise AssertionError("unknown source script shape accepted")


def test_table_profile_preserves_ids_and_only_configures_fixed_loadout():
    before = table_fixture()
    raw = profile.build_tables(before)
    after_wire, report = profile.profile_tables(raw)
    after = profile.tables(after_wire)
    assert len(report) == 2
    assert after["mp/untouched.csv"] == before["mp/untouched.csv"]
    assert after["mp/empty.csv"] == []
    for a, b in zip(before["mp/statstable.csv"], after["mp/statstable.csv"]):
        assert a[:4] == b[:4] and a[5:8] == b[5:8] and a[9:] == b[9:]
    assert {row[4] for row in after["mp/statstable.csv"][:-1] if row[4]} == set(profile.LOADOUT)
    assert after["mp/statstable.csv"][-1] == before["mp/statstable.csv"][-1]
    classes = {row[1]: row for row in after["mp/offline_classtable.csv"]}
    assert classes["201"][4:7] == ["mp5", "10", ""]
    assert classes["203"][4:7] == ["usp", "2", ""]
    assert classes["208"][4:7] == ["flash_grenade", "101", "1"]
    assert classes["205"][4] == classes["206"][4] == classes["207"][4] == "specialty_null"
    assert after["mp/offline_classtable.csv"][10:] == before["mp/offline_classtable.csv"][10:]
    assert profile.profile_tables(raw) == (after_wire, report)


def test_profile_binary_readers_reject_every_truncation_and_trailing_bytes():
    for parser, raw in ((profile.rawfiles, profile.build_rawfiles(script_fixture())),
                        (profile.tables, profile.build_tables(table_fixture()))):
        for damaged in [raw[:i] for i in range(len(raw))] + [raw + b"extra"]:
            try:
                parser(damaged)
            except (ValueError, UnicodeError):
                pass
            else:
                raise AssertionError("damaged profile input accepted")
