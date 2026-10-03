#!/usr/bin/env python3
"""Summarize bounded emulator logs without treating later frames as recovery."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


def analyze(log: bytes) -> dict:
    counts = {name: 0 for name in (
        "guestMemoryFaults", "sourceErrors", "sourceFrameFailures", "sourceSessionFailures", "mapStartupFailures",
        "nativeResourceFailureMessages", "hostExceptions", "hostExceptionsAfterTitleClosure", "activeObservations")}
    first_fault = None
    title_closed = False
    ui_observations = ui_omission_messages = ui_staging_failures = 0
    ui_maximum_omitted = 0
    ui_known_omissions = False
    guest_shutdown_complete = False
    native_captures = native_capture_failures = 0
    for line_number, line in enumerate(log.decode("utf-8", errors="replace").splitlines(), 1):
        title_closed |= "[MainWindow::on_game_closed]" in line
        guest_shutdown_complete |= "[kisak-vita] guest shutdown complete" in line
        native_captures += int("[kisak-vita] native capture complete " in line)
        native_capture_failures += int("[kisak-vita] native capture failed" in line)
        if "[kisak-vita] source UI omitted " in line:
            ui_omission_messages += 1
            ui_known_omissions = True
        ui_staging_failures += int("[kisak-vita] source UI resource staging failed" in line)
        ui = re.search(r"source UI frame=\d+ commands=\d+ quads=\d+ missing=(\d+) unsupported=(\d+) omittedTotal=(\d+)", line)
        if ui:
            missing, unsupported, omitted = map(int, ui.groups())
            ui_observations += 1
            ui_maximum_omitted = max(ui_maximum_omitted, omitted)
            ui_known_omissions |= bool(missing or unsupported or omitted)
        flags = {
            "guestMemoryFaults": (("[handle_access_violation]" in line and "Skipped " not in line)
                or bool(re.search(r"Invalid (?:exclusive )?(?:read|write) of uint\d+_t at addr", line))),
            "sourceErrors": "[kisak-source-error]" in line,
            "sourceFrameFailures": "[kisak-vita] source frame failed" in line,
            "sourceSessionFailures": "[kisak-vita] source session failed" in line,
            "mapStartupFailures": any(message in line for message in (
                "[kisak-vita] map load failed", "[kisak-vita] map DM init failed",
                "[kisak-vita] source UI resource staging failed")),
            "nativeResourceFailureMessages": ("native texture upload failed" in line
                or bool(re.search(r"gpu_alloc_\w+ failed with a requested size", line))),
            "hostExceptions": "[logging::exception_handler]: Exception" in line,
            "activeObservations": "[kisak-vita] perf frames=" in line and "sourceState=active" in line,
        }
        for key, matched in flags.items():
            counts[key] += int(matched)
        if flags["hostExceptions"] and title_closed:
            counts["hostExceptionsAfterTitleClosure"] += 1
        if first_fault is None and any(flags[key] for key in flags if key != "activeObservations"):
            first_fault = {"line": line_number, "text": line[:512]}
    return {
        "schemaVersion": 1, "stdoutSha256": hashlib.sha256(log).hexdigest(),
        **counts, "firstFault": first_fault,
        "guestRuntimeFaultFree": (False if any(counts[key] for key in
            ("guestMemoryFaults", "sourceErrors", "sourceFrameFailures", "sourceSessionFailures", "mapStartupFailures", "nativeResourceFailureMessages")) else
            True if counts["activeObservations"] else None),
        # Title closure is a timestamp marker, not proof of a clean guest
        # shutdown or a diagnosis of which side caused a host exception.
        "sourceUi": {"frameObservations": ui_observations,
            "stagingFailures": ui_staging_failures, "omissionMessages": ui_omission_messages,
            "maximumReportedMapOmissions": ui_maximum_omitted,
            "knownOmissions": ui_known_omissions,
            "visuallyQualified": False},
        "guestShutdownCompleted": guest_shutdown_complete,
        "nativeCapture": {"completed": native_captures, "failures": native_capture_failures,
                          "visuallyQualified": False},
        "cleanExitVerified": guest_shutdown_complete and title_closed and not any(counts[key] for key in
            ("guestMemoryFaults", "sourceErrors", "sourceFrameFailures", "sourceSessionFailures",
             "mapStartupFailures", "nativeResourceFailureMessages", "hostExceptions")), "hardwareQualified": False,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-directory", type=Path, required=True)
    args = parser.parse_args()
    root = args.run_directory.resolve()
    log = (root / "stdout.txt").read_bytes()
    report = analyze(log)
    identity = root / "run.json"
    report["runIdentitySha256"] = hashlib.sha256(identity.read_bytes()).hexdigest()
    (root / "runtime-evidence.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    from analyze_vita_source_input import write_report
    write_report(root, log, report["runIdentitySha256"])


if __name__ == "__main__":
    main()
