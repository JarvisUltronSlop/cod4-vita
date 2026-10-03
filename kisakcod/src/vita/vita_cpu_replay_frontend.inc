// Shared unchanged by the CPU candidate and frozen-source replay adapter.
// Included at the outer input boundary, before source advancement/map loading.
#if KISAK_VITA_CPU_PROFILE
        if(frontend.ConsumeSourceReplayRequest()) {
            sourceSession.Stop();
            if(kisak::vita::VitaCpuReplay_BeginTape()) {
                frontend.SetSelectedMapIndex(0);
                frontend.SetDiagnosticScreen(kisak::vita::VitaFrontendScreen::Loading);
                mapLoadRequested=true;developerPerfVisible=true;
            } else {
                frontend.FailMapLoad("SOURCE REPLAY UNAVAILABLE - RESTART APP AND COPY LOGS");
            }
        }
        if(kisak::vita::VitaCpuReplay_TapeActive()) {
            const bool fatal=frontend.Screen()==kisak::vita::VitaFrontendScreen::Fatal;
            const bool cancel=inputSample.startPressed || resumedFromPlatform
                || fatal;
            if(cancel || kisak::vita::VitaCpuReplay_TapeComplete()) {
                kisak::vita::VitaCpuReplay_EndTape(!cancel);
                sourceSession.Stop();
                if(!fatal)frontend.SetDiagnosticScreen(kisak::vita::VitaFrontendScreen::Main);
                resetSourceClock=true;
#if KISAK_VITA_CPU_RUNTIME
                resetCpuSchedule=true;
#endif
            }
            const bool toggle=inputSample.performanceTogglePressed;
            inputSample={};inputSample.performanceTogglePressed=toggle;
        }
#endif
