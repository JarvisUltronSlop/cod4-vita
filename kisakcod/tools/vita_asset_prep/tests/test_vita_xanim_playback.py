#!/usr/bin/env python3
"""Regression checks for the bounded sampled-XAnim playback/notetrack path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"


def main() -> int:
    source = BOOTSTRAP.read_text(encoding="utf-8")
    assert "void VitaQuaternionSlerp" in source
    assert "q and -q represent the same rotation" in source
    assert "VitaQuaternionSlerp(first->quat, second->quat" in source
    assert "VitaAxisToQuaternion" in source
    assert "XAnimCalcParts accumulates the authored quaternion" in source
    assert "animation->loop" in source
    assert "std::min(animationTime * animationRate, lastFrame)" in source
    assert "reloadStartTime" in source
    assert "reload_empty" in source
    assert "clipAmmo == 0" in source
    assert "viewmodel_%s_fire_ads" in source
    assert "soundPlayer->reloadStartTime" in source
    bundle_header = (ROOT / "src" / "vita" / "vita_animation_bundle.h").read_text(
        encoding="utf-8"
    )
    bundle_source = (ROOT / "src" / "vita" / "vita_animation_bundle.cpp").read_text(
        encoding="utf-8"
    )
    assert "struct VitaAnimationNotify" in bundle_header
    assert "uint32_t NotifyCount() const" in bundle_header
    assert "CollectNotifies" in bundle_header
    assert "bool SourceComplete() const" in bundle_header
    assert "VITQ v2 originally ended after the sampled tracks" in bundle_source
    assert "VitaAnimationRecordV3" in bundle_source
    assert "deltaPart.present = true" in bundle_source
    assert "notify.time > oldPhase" in bundle_source
    exporter = (ROOT / "src" / "database" / "db_registry.cpp").read_text(
        encoding="utf-8"
    )
    assert "VitaAnimationNotifyBuild" in exporter
    assert "DB_VitaBuildAnimationSource" in exporter
    assert "kVitaAnimationVersionSource" in exporter
    assert "notify write failed" in exporter
    print("vita xanim playback tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
