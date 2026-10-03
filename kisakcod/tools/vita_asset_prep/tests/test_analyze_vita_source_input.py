import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("input_evidence", Path(__file__).resolve().parents[1] / "analyze_vita_source_input.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_frozen_stimulus_does_not_claim_weapon_activity_or_parity():
    log = (b"source input smoke loaded steps=1 minimumMs=100\n"
           b"source input phase=0 name=fire time=1000\n"
           b"source input cmd phase=0 number=10 time=1000 buttons=1 weapon=3 offhand=0 move=-127,0 angles=0,65535,0 rejected=0\n"
           b"source input state owner=client phase=0 time=1000 pm=0 flags=800 health=100 weapon=3 weaponState=0 clip=30 ammo=60 ads=0 pos=1,2,3\n"
           b"source input smoke completed steps=1 elapsedMs=110\n")
    report, records = module.extract(log)
    assert records[0]["move"] == [-127, 0] and records[0]["angles"][1] == 65535
    observed = report["phases"]["0"]["states"]["client"]
    assert observed["flags"] == [2048] and observed["clip"] == [30]
    assert observed["weaponState"] == [0]
    assert report["completed"]["steps"] == 1
    assert report["windowsVitaParityQualified"] is False


def test_incomplete_or_nonfinite_observations_stay_invalid():
    report, rows = module.extract(
        b"source input phase=0 name=fire time=1000\n"
        b"source input cmd phase=0 time=1000\n"
        b"source input state owner=client phase=0 time=1000 pm=0 flags=0 health=100 weapon=3 weaponState=0 clip=30 ammo=60 ads=nan pos=1,2,3\n")
    assert report["malformedRecords"] == 2 and rows == []
    assert report["completed"] is None
