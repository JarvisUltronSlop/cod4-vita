#!/usr/bin/env python3
"""Regression checks for the source-backed Vita MP client audit boundary."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
CMAKE = ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt"
WIN_LOCAL = ROOT / "src" / "win32" / "win_local.h"
DATABASE = ROOT / "src" / "database" / "database.h"
PROFILE = ROOT / "src" / "qcommon" / "com_playerprofile.cpp"


def main() -> int:
    cmake = CMAKE.read_text(encoding="utf-8")
    win_local = WIN_LOCAL.read_text(encoding="utf-8")
    database = DATABASE.read_text(encoding="utf-8")
    profile = PROFILE.read_text(encoding="utf-8")
    reference_launcher = (ROOT.parent / "run-windows-reference.ps1").read_text(
        encoding="utf-8"
    )
    server_probe = (ROOT / "tools" / "vita_asset_prep" /
                    "vita_server_layout_probe.cpp").read_text(encoding="utf-8")

    target = cmake.split("add_library(KisakCOD-vita-mp-client STATIC", 1)[1]
    target = target.split("target_compile_definitions", 1)[0]
    assert "src/client_mp/cl_main_mp.cpp" in target
    assert "src/client_mp/cl_parse_mp.cpp" in target
    assert "src/client_mp/cl_ui_mp.cpp" in target
    assert "src/client_mp/cl_net_chan_mp.cpp" in target
    assert "src/universal/base64.cpp" in target
    assert "KisakCOD-vita-mp-client" in cmake
    assert "KisakCOD-vita-mp-client" in cmake.split(
        "add_executable(KisakCOD-vita-script-lifecycle-probe", 1
    )[1]

    # The Vita client headers include win_local.h directly while database.h
    # also owns this ABI type; both paths must share one guarded definition.
    assert "KISAK_VITA_FAST_CRITICAL_SECTION" in win_local
    assert "KISAK_VITA_FAST_CRITICAL_SECTION" in database
    assert 'I_strncpyz(profileName, "User", sizeof(profileName));' in profile
    assert 'const char *profileOverride = std::getenv("KISAK_PROFILE_NAME")' in profile
    assert "$profileConfig = Join-Path $profileRoot 'config_mp.cfg'" in reference_launcher
    # The disposable desktop oracle uses the lower-case profile that reaches
    # command-line +map deterministically in the recovered profile parser.
    # This does not alter the Vita shell's user-facing default profile.
    assert "$profileName = 'testname'" in reference_launcher
    assert "seta name \"testname\"" in reference_launcher
    assert 'sizeof(serverStatic_t) < 8u * 1024u * 1024u' in server_probe
    print("vita client audit tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
