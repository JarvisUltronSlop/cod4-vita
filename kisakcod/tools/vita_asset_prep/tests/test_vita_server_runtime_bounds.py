"""Keep source server runtime counts inside the Vita serverStatic_t arrays."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / "src" / "server_mp" / "server_mp.h"
IMPLEMENTATION = ROOT / "src" / "server_mp" / "sv_init_mp.cpp"


def test_vita_server_client_count_is_pinned_to_storage() -> None:
    source = IMPLEMENTATION.read_text(encoding="utf-8")
    assert "iassert(minimum <= KISAK_SVS_MAX_CLIENTS)" in source
    assert 'Dvar_SetInt(uiMaxClients, KISAK_SVS_MAX_CLIENTS)' in source
    assert "Dvar_SetInt(const_cast<dvar_s*>(sv_maxclients), KISAK_SVS_MAX_CLIENTS)" in source
    assert "sv_maxclients->current.integer <= KISAK_SVS_MAX_CLIENTS" in source
    assert "#if defined(KISAK_VITA_SLICE)\n    SV_BoundMaxClients(1);" in source


def test_snapshot_runtime_counts_use_compiled_capacities() -> None:
    source = IMPLEMENTATION.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    assert "svs.numSnapshotEntities = KISAK_SVS_SNAPSHOT_ENTITIES" in source
    assert "requestedClients <= KISAK_SVS_SNAPSHOT_CLIENTS" in source
    assert "#define KISAK_SVS_MAX_CLIENTS 1" in header
    assert "#define KISAK_SVS_SNAPSHOT_ENTITIES 4096" in header
    assert "#define KISAK_SVS_SNAPSHOT_CLIENTS 2048" in header


def test_spawn_copies_string_dvar_through_its_string_lane() -> None:
    source = IMPLEMENTATION.read_text(encoding="utf-8")
    assert "I_strncpyz(sv.gametype, sv_gametype->current.string, 64)" in source
    assert "(char *)sv_gametype->current.integer" not in source


if __name__ == "__main__":
    test_vita_server_client_count_is_pinned_to_storage()
    test_snapshot_runtime_counts_use_compiled_capacities()
    test_spawn_copies_string_dvar_through_its_string_lane()
    print("vita server runtime bounds tests: ok")
