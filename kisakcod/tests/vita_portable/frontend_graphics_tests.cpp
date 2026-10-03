#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_frontend.h>
#include <vita/vita_frontend_texture_lease.h>
#include <cassert>
#include <cstdlib>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <gfx_vita/gfx_vita_backend.h>
using namespace kisak::vita;
bool GfxVita_IsSoftwareFallback() { return false; }
int main() {
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    VitaFrontend menu; menu.Reset(); menu.SetAvailableMapMask((1u<<0)|(1u<<2));
    {
        static std::vector<uint32_t> destroyed;
        GfxBackendApi textures{};
        textures.DestroyTexture=+[](GfxTextureHandle item){destroyed.push_back(item.value);};
        VitaFrontendTextureLease common;
        std::vector<GfxTextureHandle> owned={{1},{2},{3},{4},{5},{2}};
        assert(common.Adopt(owned,{{1},{2},{3},{4}}));
        assert(owned.size()==1 && owned[0].value==5);
        for(unsigned cycle=0;cycle<10;++cycle) {
            for(auto handle:owned)textures.DestroyTexture(handle);
            owned={{10+cycle}}; // Replace only this map's assets.
            assert(common.Adopt(owned,{{1},{2},{3},{4}}));
        }
        for(auto value:destroyed)assert(value>4);
        const auto unchanged=owned;
        assert(!common.Adopt(owned,{{999}})); // Missing ownership cannot transfer.
        assert(owned.size()==unchanged.size() && owned[0].value==unchanged[0].value);
        owned.push_back({99});assert(!common.Adopt(owned,{{99}})); // Bounded lease is full.
        assert(owned.size()==2);
        common.Release(&textures);common.Release(&textures);
        for(unsigned handle=1;handle<=4;++handle)
            assert(std::count(destroyed.begin(),destroyed.end(),handle)==1);
    }
#if KISAK_VITA_CPU_RUNTIME
    static unsigned bindings=0,pixels=0;static uint32_t texture=99;
    GfxBackendApi glyphApi{};
    glyphApi.BindTexture=+[](GfxTextureHandle h){++bindings;texture=h.value;};
    glyphApi.SetSurfaceMode=+[](GfxSurfaceMode mode){assert(mode==GfxSurfaceMode::Overlay);};
    glyphApi.DrawIndexed=+[](const GfxDrawVertex* v,uint32_t count,const uint16_t* ix,uint32_t indices){
        assert(texture==0 && count==4 && indices==6);++pixels;
        assert(ix[0]==0 && ix[1]==1 && ix[2]==2 && ix[3]==0 && ix[4]==2 && ix[5]==3);
        for(unsigned i=0;i<4;++i)assert(v[i].color[0]==.88f && v[i].color[3]==1.0f);
    };
    menu.DrawCpuLoadingStatus(&glyphApi,"LOADING ITEMS",12000000);
    assert(pixels>100 && bindings==4); // one initial bind, then one per text run
    static unsigned artDraws=0, statusDraws=0;
    glyphApi.DrawIndexed=+[](const GfxDrawVertex* v,uint32_t count,const uint16_t*,uint32_t indices){
        assert(count==4 && indices==6);
        if(texture==77) {
            ++artDraws;
            assert(v[0].position[0]==-1 && v[0].position[1]==-1);
            assert(v[2].position[0]==1 && v[2].position[1]==1);
            assert(v[0].texCoord[0]==0 && v[2].texCoord[0]==1);
            assert(v[0].color[0]==1 && v[0].color[3]==1);
        } else { assert(texture==0);++statusDraws; }
    };
    menu.SetLoadingBackground({77});menu.SetMenuBackground({88});
    menu.SetDiagnosticScreen(VitaFrontendScreen::Loading);
    menu.SetLoadingStage("CONNECTING LOCAL PLAYER",0,0);
    for(unsigned i=0;i<60;++i) {
        VitaInputSample in{};in.menuAcceptPressed=in.menuBackPressed=in.startPressed=true;
        assert(!menu.HandleInput(in,true));menu.Update();
        assert(menu.Screen()==VitaFrontendScreen::Loading);
        menu.Draw(&glyphApi); // Every stage draws only map art and safe glyphs.
    }
    assert(artDraws==60 && statusDraws>100);
    menu.CompleteMapLoad();menu.Update();assert(menu.InGame());
    const unsigned completeDraws=artDraws;menu.Draw(&glyphApi);assert(artDraws==completeDraws);
    // A failure after asset teardown must not sample stale menu/font handles.
    glyphApi.DrawIndexed=+[](const GfxDrawVertex*,uint32_t count,const uint16_t*,uint32_t indices){
        assert(texture==0 && count==4 && indices==6);++statusDraws;
    };
    menu.SetMenuBackground({UINT32_MAX});menu.SetFont(nullptr,{UINT32_MAX});
    menu.SetMenuButtonTextures({UINT32_MAX},{UINT32_MAX});
    menu.FailMapLoad("PRIMARY LIGHTS DECODE FAILED - COPY LOGS");
    const unsigned beforeFailure=statusDraws;
    menu.Draw(&glyphApi);assert(statusDraws>beforeFailure+100);
    assert(menu.Screen()==VitaFrontendScreen::Fatal);
    VitaInputSample accept{};accept.menuAcceptPressed=true;
    assert(menu.HandleInput(accept,true) && menu.Screen()==VitaFrontendScreen::Main);
    assert(!menu.ConsumeShipmentTestRequest() && !menu.ConsumeCpuDiagnosticRequest());
    menu.Reset();menu.SetAvailableMapMask((1u<<0)|(1u<<2));
#endif
    const auto press=[&](auto member) {VitaInputSample in{};in.*member=true;return menu.HandleInput(in,true);};
    assert(menu.Screen()==VitaFrontendScreen::Main);
    assert(menu.RenderSettings().resolution==VitaWorldResolution::Balanced && menu.RenderSettings().balancedGrass);
    assert(!menu.ConsumeComparisonRequest());
    press(&VitaInputSample::menuDownPressed); press(&VitaInputSample::menuAcceptPressed);
    assert(menu.Screen()==VitaFrontendScreen::Options);
    press(&VitaInputSample::menuDownPressed); press(&VitaInputSample::menuAcceptPressed);
    assert(menu.Screen()==VitaFrontendScreen::Graphics);
    press(&VitaInputSample::menuRightPressed); assert(menu.RenderSettings().resolution==VitaWorldResolution::Performance);
    press(&VitaInputSample::menuRightPressed); assert(menu.RenderSettings().resolution==VitaWorldResolution::Native);
    press(&VitaInputSample::menuLeftPressed); assert(menu.RenderSettings().resolution==VitaWorldResolution::Performance);
    press(&VitaInputSample::menuDownPressed); press(&VitaInputSample::menuAcceptPressed);
    assert(!menu.RenderSettings().balancedGrass);
#if KISAK_VITA_CPU_RUNTIME
    assert(menu.RenderSettings().simpleWorldLighting);
    press(&VitaInputSample::menuDownPressed); press(&VitaInputSample::menuAcceptPressed);
    assert(!menu.RenderSettings().simpleWorldLighting);
    press(&VitaInputSample::menuRightPressed);assert(menu.RenderSettings().simpleWorldLighting);
    assert(menu.Screen()==VitaFrontendScreen::Graphics && !menu.ConsumeComparisonRequest());
#endif
    press(&VitaInputSample::menuDownPressed); press(&VitaInputSample::menuAcceptPressed);
    assert(menu.ConsumeComparisonRequest()); assert(!menu.ConsumeComparisonRequest());
    assert(menu.Screen()==VitaFrontendScreen::Main);
    // Both maps and pause/options retain pending settings without mutating a running map.
#if KISAK_VITA_CPU_PROFILE
    press(&VitaInputSample::menuDownPressed);press(&VitaInputSample::menuDownPressed);
    press(&VitaInputSample::menuAcceptPressed);
    assert(menu.Screen()==VitaFrontendScreen::Tests);
    // Overlay input is never a test request, including at Main and Tests.
    press(&VitaInputSample::performanceTogglePressed);
    assert(!menu.ConsumeShipmentTestRequest() && !menu.ConsumeCpuDiagnosticRequest());
    assert(!menu.ConsumeSourceReplayRequest() && !menu.ConsumeComparisonRequest());
#if KISAK_VITA_CPU_RUNTIME
    // Three stable tests. Down wraps Profile -> CPU and Up CPU -> Profile;
    // no historical experiment or Back row may reappear in this route.
    for(unsigned row=0;row<3;++row) {
        menu.SetDiagnosticScreen(VitaFrontendScreen::Tests);
        for(unsigned i=0;i<row;++i)press(&VitaInputSample::menuDownPressed);
        press(&VitaInputSample::menuAcceptPressed);
        assert(menu.Screen()==VitaFrontendScreen::Main);
        assert(menu.ConsumeCpuDiagnosticRequest()==(row==0?1u:row==1?37u:35u));
        assert(!menu.ConsumeCpuDiagnosticRequest() && !menu.ConsumeShipmentTestRequest());
        assert(!menu.ConsumeSourceReplayRequest() && !menu.ConsumeComparisonRequest());
    }
    menu.SetDiagnosticScreen(VitaFrontendScreen::Tests);
    press(&VitaInputSample::menuDownPressed);press(&VitaInputSample::menuDownPressed);press(&VitaInputSample::menuDownPressed);press(&VitaInputSample::menuAcceptPressed);
    assert(menu.ConsumeCpuDiagnosticRequest()==1u);
    menu.SetDiagnosticScreen(VitaFrontendScreen::Tests);
    press(&VitaInputSample::menuUpPressed);press(&VitaInputSample::menuAcceptPressed);
    assert(menu.ConsumeCpuDiagnosticRequest()==35u);
#else
    press(&VitaInputSample::menuAcceptPressed);
    assert(menu.ConsumeShipmentTestRequest() && !menu.ConsumeShipmentTestRequest());
#endif
    menu.SetDiagnosticScreen(VitaFrontendScreen::Tests);
    press(&VitaInputSample::menuBackPressed);assert(menu.Screen()==VitaFrontendScreen::Main);
    press(&VitaInputSample::menuAcceptPressed);assert(menu.Screen()==VitaFrontendScreen::Tests);
    press(&VitaInputSample::menuAcceptPressed);menu.Reset();
    assert(!menu.ConsumeShipmentTestRequest() && !menu.ConsumeCpuDiagnosticRequest());
#endif
    for(unsigned map:{0u,2u}) {
        menu.SetSelectedMapIndex(map); menu.SetDiagnosticScreen(VitaFrontendScreen::Loading); menu.CompleteMapLoad();
        for(int i=0;i<45;++i) menu.Update();
        assert(menu.InGame()); menu.OpenPause(); assert(menu.Screen()==VitaFrontendScreen::Pause);
        press(&VitaInputSample::menuDownPressed); press(&VitaInputSample::menuAcceptPressed);
        assert(menu.Screen()==VitaFrontendScreen::Options);
        press(&VitaInputSample::menuDownPressed); press(&VitaInputSample::menuAcceptPressed);
        assert(menu.Screen()==VitaFrontendScreen::Graphics);
        press(&VitaInputSample::menuBackPressed); press(&VitaInputSample::menuBackPressed);
        assert(menu.Screen()==VitaFrontendScreen::Pause);
        press(&VitaInputSample::menuBackPressed); assert(menu.InGame());
        assert(menu.RenderSettings().resolution==VitaWorldResolution::Performance);
    }
    // The original IDs retain their DLC gaps. Every installed original map
    // follows the ordinary FFA transition; excluded and absent packs cannot.
    menu.Reset(); menu.SetAvailableMapMask(UINT32_MAX);
    unsigned originals=0;
    for(uint32_t map=0;map<kVitaMpMapCount;++map) {
        const bool original=map!=13 && map!=14 && map!=17 && map!=18;
        assert(VitaMapIsEnabled(map)==original);
        uint32_t resolved=99;
        const std::string name="mp_"+std::string(kVitaMpMaps[map].name);
        assert(VitaFindMapIndex(name.c_str(),&resolved)==original);
        assert(VitaFindMapIndex(kVitaMpMaps[map].name,&resolved)==original);
        assert(bool(VitaMapPackagePath(map))==original);
        if(!original)continue;
        ++originals; assert(resolved==map);
        const std::string expected="ux0:data/CODVIT001/paks/"+name+".kvap";
        assert(expected==VitaMapPackagePath(map));
        menu.SetSelectedMapIndex(map);menu.SetDiagnosticScreen(VitaFrontendScreen::MapSelect);
        menu.SetSelectedMapIndex(map);press(&VitaInputSample::menuAcceptPressed);
        assert(menu.Screen()==VitaFrontendScreen::Loading && menu.SelectedMapIndex()==map);
        menu.FailMapLoad("TEST CONTENT FAILURE");
        press(&VitaInputSample::menuBackPressed);assert(menu.Screen()==VitaFrontendScreen::Main);
    }
    assert(originals==16);
    for(const char* invalid:{"", "MP_CRASH", "mp_crash_snow", "../mp_crash", "mp_crash;quit"}) {
        uint32_t untouched=99;assert(!VitaFindMapIndex(invalid,&untouched) && untouched==99);
    }
    assert(!VitaMapIsEnabled(20) && !VitaMapIsEnabled(UINT32_MAX));
    assert(!VitaMapPackagePath(20) && !VitaMapPackagePath(UINT32_MAX));
    uint32_t unused=99;assert(!VitaFindMapIndex(nullptr,&unused) && !VitaFindMapIndex("crash",nullptr));
    menu.SetAvailableMapMask(1);menu.SetSelectedMapIndex(0);
    menu.SetMapAvailabilityError(1,"INCOMPATIBLE COMMON PACK");
    menu.SetDiagnosticScreen(VitaFrontendScreen::MapSelect);
    press(&VitaInputSample::menuDownPressed);assert(menu.SelectedMapIndex()==1);
    press(&VitaInputSample::menuAcceptPressed);assert(menu.Screen()==VitaFrontendScreen::MapSelect);
    // A rejected map cannot poison the next Shipment load.
    press(&VitaInputSample::menuUpPressed);press(&VitaInputSample::menuAcceptPressed);
    assert(menu.Screen()==VitaFrontendScreen::Loading && menu.SelectedMapIndex()==0);
    menu.Reset();menu.SetAvailableMapMask(0);menu.SetDiagnosticScreen(VitaFrontendScreen::MapSelect);
    press(&VitaInputSample::menuAcceptPressed);assert(menu.Screen()!=VitaFrontendScreen::Loading);
    // Back remains operable even when there is no installed map.
    press(&VitaInputSample::menuBackPressed);assert(menu.Screen()!=VitaFrontendScreen::MapSelect);
}
