#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_pass_isolation.h>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    using namespace kisak::vita;
    for(unsigned phase=1;phase<=9;++phase) {
        VitaPassIsolation gate;gate.Begin(kVitaPassIsolation|phase);
        VitaRenderPassProbe probe;probe.Begin(kVitaPassIsolation|phase,0);
        assert(probe.passIsolation && probe.WorldAlphaActive() && !probe.Fenced() && !probe.drawProbe);
        assert(!probe.composition && !probe.graphicsProfile && !probe.worldComponents);
        assert(VitaWeaponSpecularEnabled(probe.requestedPhase) && VitaWeaponVertexEnabled(probe.requestedPhase));
        for(unsigned pass=0;pass<7;++pass) {
            const bool suppressed=(phase==2 && pass==1)||(phase==4 && pass==2)||(phase==6 && pass==4);
            for(unsigned draw=0;draw<30;++draw)assert(gate.Draw(pass,false)==!suppressed);
            assert(gate.prepared[pass]==30 && gate.suppressed[pass]==(suppressed?30u:0u));
            assert(gate.Draw(pass,true)); // no category can suppress a target clear
        }
        assert(gate.CopyPost()==(phase==8));assert(gate.Draw(7,false) && gate.errors==1);
        gate.Begin(0);assert(!gate.phase && !gate.errors && gate.Draw(1,false));
        for(auto n:gate.prepared)assert(n==0);
    }
    assert(VitaPassIsolationPhase(0)==1 && VitaPassIsolationPhase(19999)==1 && VitaPassIsolationPhase(20000)==2);
    assert(VitaPassIsolationPhase(179999)==9 && VitaPassIsolationPhase(180000)==0);
    for(auto n:{0u,kVitaPassIsolation,kVitaPassIsolation+10u,0xffffffffu})assert(!VitaPassIsolationMode(n));
    assert(argc==2);std::ifstream in(argv[1]);assert(in);std::string s((std::istreambuf_iterator<char>(in)),{});
    unsigned wrappers=0;size_t pos=0;while((pos=s.find("GfxVita_SceneSubmissionEnabled() && GfxVita_ProfileDrawSubmission()",pos))!=std::string::npos){++wrappers;++pos;}
    assert(wrappers==3);assert(s.find("g_passIsolation.Draw(GpuDrawPass(),g_nativeClearDraw)")!=std::string::npos);
    assert(s.find("!g_renderPassProbe.passIsolation);")!=std::string::npos);
    assert(s.find("glBindTexture(GL_TEXTURE_2D,g_nativePost.sceneTexture);glUniform1i(g_worldCopy.sampler,0)")!=std::string::npos);
}
