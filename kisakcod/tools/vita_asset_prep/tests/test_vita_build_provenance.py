import importlib.util
from pathlib import Path

import pytest


spec = importlib.util.spec_from_file_location(
    "record_vita_build", Path(__file__).resolve().parents[1] / "record_vita_build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def test_release_rejects_reachable_capture_despite_optimized_code():
    with pytest.raises(ValueError, match="Release retains diagnostic"):
        build.validate_release_symbols(
            "810045f0 T GfxVita_QueueProfileCapture(kisak::vita::VitaRenderPacket const&)\n")
    build.validate_release_symbols("81004760 T CG_DrawActiveFrame(int, int, int, int)\n")


@pytest.mark.parametrize("configuration,profile,accepted", [
    ("Release", "OFF", True), ("Release", "ON", False),
    ("Profile", "ON", True), ("Profile", "OFF", False),
])
def test_profile_option_matches_requested_build(configuration, profile, accepted):
    cache = {
        "CMAKE_BUILD_TYPE": "Release" if configuration == "Release" else "RelWithDebInfo",
        "CMAKE_CXX_FLAGS_RELEASE": "-O3 -DNDEBUG",
        "CMAKE_CXX_FLAGS_RELWITHDEBINFO": "-O3 -g -DNDEBUG",
        "KISAK_VITA_PROFILE": profile,
        "KISAK_VITA_SOURCE_RUNTIME": "ON",
        "KISAK_VITA_EXTERNAL_CONTENT": "ON",
    }
    if accepted:
        build.validate_configuration(cache, configuration)
    else:
        with pytest.raises(ValueError, match="profiling instrumentation"):
            build.validate_configuration(cache, configuration)
