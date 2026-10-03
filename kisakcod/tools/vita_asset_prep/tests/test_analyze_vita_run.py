import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("analyze_vita_run", Path(__file__).resolve().parents[1] / "analyze_vita_run.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_later_frames_do_not_erase_earlier_guest_fault():
    report = module.analyze(b"[handle_access_violation]: bad guest read\n"
                            b"[kisak-vita] perf frames=60 sourceState=active\n")
    assert report["guestRuntimeFaultFree"] is False
    assert report["firstFault"]["line"] == 1 and report["guestMemoryFaults"] == 1
    diagnostic = module.analyze(b"[ArmDynarmicCallback::MemoryRead]: Invalid read of uint8_t at address: 0xc\n")
    assert diagnostic["guestMemoryFaults"] == 1
    assert diagnostic["guestRuntimeFaultFree"] is False


def test_no_active_evidence_is_unavailable_and_host_closure_is_separate():
    assert module.analyze(b"")["guestRuntimeFaultFree"] is None
    report = module.analyze(b"[kisak-vita] perf frames=60 sourceState=active\n"
                            b"[MainWindow::on_game_closed]: title closed\n"
                            b"[logging::exception_handler]: Exception ACCESS_VIOLATION\n")
    assert report["guestRuntimeFaultFree"] is True
    assert report["hostExceptionsAfterTitleClosure"] == 1
    assert report["cleanExitVerified"] is False and report["hardwareQualified"] is False


def test_later_map_ui_cannot_erase_omitted_presentation():
    report = module.analyze(b"[kisak-vita] source UI frame=120 commands=6 quads=20 missing=0 unsupported=1 omittedTotal=30\n"
        b"[kisak-vita] source UI frame=1 commands=2 quads=8 missing=0 unsupported=0 omittedTotal=0\n")
    assert report["sourceUi"]["knownOmissions"]
    assert report["sourceUi"]["maximumReportedMapOmissions"] == 30
    assert report["sourceUi"]["frameObservations"] == 2
    assert not report["sourceUi"]["visuallyQualified"]


def test_native_gpu_allocation_failure_is_not_a_successful_run():
    report = module.analyze(b"gpu_alloc_mapped_aligned_for_gpu failed with a requested size of 336 bytes\n"
        b"[kisak-vita] perf frames=60 sourceState=active\n")
    assert report["nativeResourceFailureMessages"] == 1
    assert not report["guestRuntimeFaultFree"]
    assert report["firstFault"]["line"] == 1


def test_active_server_does_not_qualify_a_failed_map_start():
    for failure in (b"map DM init failed index=0 name=shipment",
                    b"map load failed: post-source semantic residency",
                    b"source UI resource staging failed"):
        report = module.analyze(b"[kisak-vita] " + failure + b"\n"
            b"[kisak-vita] perf frames=60 sourceState=active\n"
            b"[kisak-vita] guest shutdown complete\n[MainWindow::on_game_closed]: closed\n")
        assert report["mapStartupFailures"] == 1
        assert report["guestRuntimeFaultFree"] is False
        assert report["cleanExitVerified"] is False
        assert report["firstFault"]["line"] == 1


def test_guest_shutdown_marker_cannot_hide_teardown_assertions():
    complete = b"[kisak-vita] guest shutdown complete\n[MainWindow::on_game_closed]: title closed\n"
    assert module.analyze(complete)["cleanExitVerified"]
    failed = module.analyze(b"[kisak-source-error] source assertion refCount\n" + complete)
    assert failed["guestShutdownCompleted"] and not failed["cleanExitVerified"]
    capture = module.analyze(b"[kisak-vita] native capture complete sourceTime=21000 rendererFrame=300\n")
    assert capture["nativeCapture"]["completed"] == 1 and not capture["nativeCapture"]["visuallyQualified"]
