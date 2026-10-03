#if KISAK_VITA_CPU_PROFILE && KISAK_VITA_SOURCE_RUNTIME
        {
            auto& test=kisak::vita::shipmentAutoTest;
            bool mayStart=frontend.Screen()==kisak::vita::VitaFrontendScreen::Main;
#if KISAK_VITA_BENCHMARK
            mayStart=mayStart && !benchmark.Active();
#endif
#if KISAK_VITA_CPU_RUNTIME
            mayStart=mayStart && !cpuDiagnostic && !kisak::vita::VitaCpuReplay_TapeActive();
#endif
            if(test.Request(mayStart,frontend.ConsumeShipmentTestRequest(),sceKernelGetProcessTimeWide())) {
                // Consume the explicit menu request before source advancement.
                inputSample={};sourceSession.Stop();
                frontend.ConsumeComparisonRequest();
#if KISAK_VITA_CPU_RUNTIME
                frontend.ConsumeCpuDiagnosticRequest();frontend.ConsumeSourceReplayRequest();
#endif
                developerPerfVisible=true;perfOverlayRefreshCountdown=0;scoreboardVisible=false;
                frontend.SetSelectedMapIndex(0);
                frontend.SetDiagnosticScreen(kisak::vita::VitaFrontendScreen::Loading);
                mapLoadRequested=true;
                kisak::vita::VitaCpuProfile_Counter("shipment-test-request",test.Run(),test.kDurationUs);
                kisak::vita::VitaScriptServicesTrace("shipment_test_request","fully_rendered_180_seconds");
            }
            if(test.Active()) {
                using Test=kisak::vita::VitaShipmentAutoTest;
                const bool fatal=frontend.Screen()==kisak::vita::VitaFrontendScreen::Fatal;
                const bool toggle=inputSample.performanceTogglePressed;
                const auto event=test.Tick(sceKernelGetProcessTimeWide(),
                    frontend.InGame() && sourceSession.Active() && sourceSession.Status().initialPlayerSpawned,
                    fatal,inputSample.startPressed,resumedFromPlatform);
                inputSample={};inputSample.performanceTogglePressed=toggle;
                if(event==Test::Event::Began) {
                    kisak::vita::VitaCpuProfile_Counter("shipment-test-begin",test.Run(),test.kDurationUs);
                    kisak::vita::VitaCpuProfile_Counter("shipment-test-load-us",test.Run(),test.LoadUs());
                }
                if(event==Test::Event::Completed || event==Test::Event::Cancelled || event==Test::Event::Failed) {
                    const char* ending=event==Test::Event::Completed?"shipment-test-complete":
                        event==Test::Event::Cancelled?"shipment-test-cancelled":"shipment-test-failed";
                    // Match exit retires copied frames/GPU work and sound first.
                    // The completion banner is drawn only after buffered records flush.
                    sourceSession.Stop();mapLoadRequested=false;
                    if(!fatal)frontend.SetDiagnosticScreen(kisak::vita::VitaFrontendScreen::Main);
                    resetSourceClock=true;
#if KISAK_VITA_CPU_RUNTIME
                    resetCpuSchedule=true;
#endif
                    kisak::vita::VitaCpuProfile_Counter(ending,test.Run(),test.ElapsedUs());
                    kisak::vita::VitaCpuProfile_Counter("shipment-test-skipped-phases",test.Run(),test.SkippedPhases());
                    kisak::vita::VitaScriptServicesTrace(ending,"copy_logs_before_relaunch");
                    shipmentTrace.Marker(ending);
                    const bool perfSaved=perfWriter.Write(perfRun.Summarize(),ending);
                    SyncAppLifecycle();
                    kisak::vita::VitaCpuProfile_Flush(true);
                    const auto health=kisak::vita::VitaCpuProfile_Read();
                    test.SetLogFailure(!perfSaved || !health.active || health.dropped || health.ioFailed);
                    inputSample={};
                } else if(test.Status()==Test::State::Running) {
                    const auto& drive=test.Input();
                    inputSample.forwardmove=drive.forward;inputSample.rightmove=drive.right;
                    inputSample.lookDegrees[1]=drive.yawDegrees;
                    if(drive.fire)inputSample.buttons|=BUTTON_ATTACK;
                    if(drive.ads)inputSample.buttons|=BUTTON_ADS;
                    if(drive.reload)inputSample.buttons|=BUTTON_RELOAD;
                    if(drive.crouch)inputSample.buttons|=BUTTON_CROUCH;
                    if(drive.jump)inputSample.buttons|=BUTTON_JUMP;
                    if(drive.fragHeld)inputSample.buttons|=BUTTON_FRAG;
                    inputSample.weaponCyclePressed=drive.cycle;
                    inputSample.rearFragHeld=drive.fragHeld;inputSample.rearFragPressed=drive.fragPressed;
                    if(test.PhaseChanged())kisak::vita::VitaCpuProfile_Counter("shipment-test-phase",test.Phase(),test.SkippedPhases());
                    if(test.ReportDue())kisak::vita::VitaCpuProfile_Counter("shipment-test-progress",test.Run(),test.ElapsedUs());
                }
            }
        }
#endif
