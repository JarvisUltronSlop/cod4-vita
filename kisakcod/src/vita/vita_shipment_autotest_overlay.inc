#if KISAK_VITA_CPU_PROFILE && KISAK_VITA_SOURCE_RUNTIME
        {
            // Keep menu rows readable while stats remain enabled. Gameplay
            // retains the full overlay; menus use the free right-hand area.
            if(developerPerfVisible && !frontend.InGame()) {
                GfxVita_BeginNativeHud();
                const float panel[4]={0.008f,0.012f,0.018f,0.88f};
                const float color[4]={0.90f,0.95f,1.0f,1.0f};
                DrawHudPhysicalRect(gfx,516,12,432,188,0.055f,panel);
                char lines[8][112]={"VITA PERFORMANCE - LAST GAMEPLAY SAMPLE"};
                const auto& frame=latestPerfFrame;
                if(frame.frameUsec) {
                    const auto fps10=10000000u/frame.frameUsec;
                    std::snprintf(lines[1],sizeof(lines[1]),"%u.%u FPS  FRAME %u MS  P95 %u MS",
                        fps10/10,fps10%10,frame.frameUsec/1000,perfOverlaySummary.p95FrameUsec/1000);
                } else std::snprintf(lines[1],sizeof(lines[1]),"NO COMPLETED GAMEPLAY FRAME YET");
                std::snprintf(lines[2],sizeof(lines[2]),"SERVER %u  CLIENT %u  AUDIO %u US",
                    frame.sourceServerUsec,frame.sourceClientUsec,frame.audioUsec);
                std::snprintf(lines[3],sizeof(lines[3]),"DRAW %u  GRAPHICS WAIT %u US",frame.drawSubmitUsec,frame.swapWaitUsec);
                std::snprintf(lines[4],sizeof(lines[4]),"DRAW CALLS %u  TRIANGLES %u",frame.drawCalls,frame.triangles);
                std::snprintf(lines[5],sizeof(lines[5]),"ARM %d  BUS %d  GPU %d  XBAR %d",armClock,busClock,gpuClock,gpuXbarClock);
#if KISAK_VITA_CPU_RUNTIME
                std::snprintf(lines[2],sizeof(lines[2]),"SERVER %u.%u HZ  CLIENT %u.%u HZ",
                    cpuOverlay.serverRate10/10,cpuOverlay.serverRate10%10,cpuOverlay.clientRate10/10,cpuOverlay.clientRate10%10);
                std::snprintf(lines[3],sizeof(lines[3]),"CPU PREP P95 %u  P99 %u  MAX %u US",cpuOverlay.p95,cpuOverlay.p99,cpuOverlay.maximum);
                std::snprintf(lines[4],sizeof(lines[4]),"SNAP AGE %llu US  DEADLINE MISSES %llu",
                    (unsigned long long)cpuOverlay.renderer.ageUs,(unsigned long long)cpuOverlay.misses);
                std::snprintf(lines[5],sizeof(lines[5]),"PCM %u  UNDERRUN %llu  HEAP HIGH %llu KB",
                    cpuOverlay.audio.queuedBlocks,(unsigned long long)cpuOverlay.audio.underruns,
                    (unsigned long long)(cpuOverlay.heapHighWater/1024));
#endif
                const auto recording=kisak::vita::VitaCpuProfile_Read();
                std::snprintf(lines[6],sizeof(lines[6]),"LOG DROPS %llu  IO %s",
                    (unsigned long long)recording.dropped,recording.ioFailed?"FAILED":"OK");
                std::snprintf(lines[7],sizeof(lines[7]),"SELECT + START: TOGGLE STATS");
                for(unsigned i=0;i<8;++i) {
                    const float width=HudTextWidthPixels(lines[i],uiFont,0.24f);
                    const float scale=0.24f*std::min(1.0f,410.0f/std::max(1.0f,width));
                    DrawHudVirtualText(gfx,lines[i],526,33+22.0f*i,5,5,scale,false,color,0.045f,uiFont,uiFontTexture);
                }
            }
            char testLabel[128];
            kisak::vita::shipmentAutoTest.Label(testLabel,sizeof(testLabel),
                frontend.Screen()==kisak::vita::VitaFrontendScreen::Main);
            if(testLabel[0]) {
                GfxVita_BeginNativeHud();
                static const float testColor[4]={1.0f,0.9f,0.45f,1.0f};
                DrawHudVirtualText(gfx,testLabel,12,450,0,0,0.25f,false,testColor,0.01f,
                    uiFont,uiFontTexture);
            }
        }
#endif
