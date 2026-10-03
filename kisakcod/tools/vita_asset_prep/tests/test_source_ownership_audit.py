from __future__ import annotations

import importlib.util
import hashlib
import tempfile
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "audit_vita_source_ownership.py"
SPEC = importlib.util.spec_from_file_location("audit_vita_source_ownership", MODULE)
assert SPEC and SPEC.loader
audit_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit_module)


def _source_symbols() -> str:
    return "\n".join(
        needle for needles in audit_module.REQUIRED_GROUPS.values()
        for needle in needles
    )


def _link_map() -> str:
    return "Linker script and memory map\n" + "\n".join(
        f" .text.symbol 0x1000 0x20 libKisakCOD-source.a({provider})\n    0x1000 {symbol}"
        for symbol, provider in audit_module.SOURCE_PROVIDERS.items())


def test_source_owned_link_passes() -> None:
    result = audit_module.audit(_source_symbols(), _link_map())
    assert result["passed"] is True
    assert result["missingRequiredGroups"] == []
    assert result["forbiddenPrototypeGroups"] == {}
    assert result["behaviorQualified"] is False


def test_symbol_presence_cannot_certify_implementation_provider() -> None:
    result = audit_module.audit(_source_symbols())
    assert result["passed"] is False
    assert result["symbolPresencePassed"] is True
    result = audit_module.audit(_source_symbols(), _link_map().replace(
        "cg_predict_mp.cpp.obj", "vita_client_stub.cpp.obj"))
    assert result["passed"] is False
    assert "client_prediction" in result["unverifiedProviderGroups"]
    assert result["simulationLinkPassed"] is True


def test_discarded_sections_cannot_supply_a_provider() -> None:
    discarded = _link_map().replace("Linker script and memory map", "Discarded input sections")
    result = audit_module.audit(_source_symbols(), discarded)
    assert result["passed"] is False


def test_prototype_reachability_fails_even_with_source_symbols() -> None:
    symbols = _source_symbols() + "\nShipmentDmServer::HandleWeapon("
    result = audit_module.audit(symbols)
    assert result["passed"] is False
    assert "prototype_combat" in result["forbiddenPrototypeGroups"]


def test_missing_source_loops_fail() -> None:
    result = audit_module.audit("G_RunFrame(int)\nPM_Weapon(")
    assert result["passed"] is False
    assert set(result["missingRequiredGroups"]) >= {"server_frame", "client_frame"}


def test_file_hash_is_stable() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        sample = Path(temporary) / "sample.bin"
        sample.write_bytes(b"vita-source-ownership")
        assert audit_module.sha256_file(sample) == hashlib.sha256(
            b"vita-source-ownership"
        ).hexdigest()


if __name__ == "__main__":
    test_source_owned_link_passes()
    test_symbol_presence_cannot_certify_implementation_provider()
    test_discarded_sections_cannot_supply_a_provider()
    test_prototype_reachability_fails_even_with_source_symbols()
    test_missing_source_loops_fail()
    test_file_hash_is_stable()
    print("source ownership audit tests: ok")
