import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("analyze_native_inputs", Path(__file__).resolve().parents[1] / "analyze_native_inputs.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_identical_black_files_do_not_pass_native_controls(tmp_path):
    black = b"P6\n960 544\n255\n" + bytes(960 * 544 * 3)
    for name in module.CONTROL_NAMES:
        (tmp_path / f"native-contract-{name}.ppm").write_bytes(black)
    result = module.analyze(tmp_path)
    assert all(result["pixelIdenticalPairs"].values())
    assert not result["inputControlsPassed"]
    assert result["controls"]["linear"]["sampleMismatches"] == 256
    assert not result["visuallyQualified"]


def test_missing_and_truncated_captures_are_not_zero_error_evidence(tmp_path):
    result = module.analyze(tmp_path)
    assert result["controls"]["linear"]["passed"] is None
    assert result["clearCoveragePassed"] is None
    assert not result["inputControlsPassed"]
    (tmp_path / "native-contract-linear.ppm").write_bytes(b"P6\n960 544\n255\n")
    result = module.analyze(tmp_path)
    assert result["controls"]["linear"]["passed"] is False


def test_partial_clear_keeps_uncovered_pixels_visible_in_report(tmp_path):
    raw = b"\x0e\x13\x18" * (960 * 543) + bytes(960 * 3)
    (tmp_path / "native-clear.ppm").write_bytes(b"P6\n960 544\n255\n" + raw)
    result = module.analyze(tmp_path)
    assert result["clearPixelMismatches"] == 960
    assert not result["clearCoveragePassed"]


def test_depth_control_rejects_far_surface_overwriting_near_surface():
    header = b"P6\n960 544\n255\n"
    red = b"\xff\x00\x00"
    green = b"\x00\xff\x00"
    image = bytearray(red * (960 * 544))
    for y in range(136, 408):
        image[(y * 960 + 240) * 3:(y * 960 + 720) * 3] = green * 480
    assert module.check_depth_control(header + image) == 0
    assert module.check_depth_control(header + red * (960 * 544)) == 4


def test_dynamic_control_rejects_missing_or_fullscreen_geometry():
    header = b"P6\n960 544\n255\n"
    black = bytes(960 * 544 * 3)
    green = b"\x00\xff\x00" * (960 * 544)
    assert module.check_dynamic_control(header + black) == 2
    assert module.check_dynamic_control(header + green) == 2
    assert module.check_dynamic_control(header + black, crossing=True) == 3
    assert module.check_dynamic_control(header + black, crossing=True, world=True) == 1
