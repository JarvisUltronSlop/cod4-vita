#!/usr/bin/env python3
"""Regression check for the source 20-Hz / Vita 30-FPS cadence."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP = ROOT / "src" / "vita" / "vita_bootstrap.cpp"
INPUT_HEADER = ROOT / "src" / "vita" / "vita_input.h"
INPUT_SOURCE = ROOT / "src" / "vita" / "vita_input.cpp"


def main() -> int:
    source = BOOTSTRAP.read_text(encoding="utf-8")
    budget = re.search(r"constexpr uint64_t kFrameBudgetUs = (\d+);", source)
    sim_step = re.search(r"constexpr int32_t kSimulationFrameMsec = (\d+);", source)
    assert budget and int(budget.group(1)) == 33333
    assert sim_step and int(sim_step.group(1)) == 50
    assert "simulationAccumulatorUs" in source
    assert "kMaxSimulationCatchUpTicks = 5" in source
    assert "simulationTicksDue" in source
    assert "simulationAccumulatorUs -= static_cast<uint64_t>(simulationTicksDue)" in source
    assert "localServerTime += 16" not in source
    input_header = INPUT_HEADER.read_text(encoding="utf-8")
    input_source = INPUT_SOURCE.read_text(encoding="utf-8")
    assert "bool performanceTogglePressed = false;" in input_header
    assert "const bool chordHeld = sample->startHeld && sample->selectHeld;" in input_source
    assert "sample->performanceTogglePressed = chordHeld && !previousChordHeld;" in input_source
    assert "Pressed(SCE_CTRL_START) && !sample->selectHeld" in input_source
    assert "Pressed(SCE_CTRL_SELECT) && !sample->startHeld" in input_source
    assert "DrawPerformanceOverlay(" in source
    assert "perfOverlayRefreshCountdown = 15;" in source
    assert '"15 FPS FLOOR %s  CLOCK %s' in source
    print("vita frame pacing tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
