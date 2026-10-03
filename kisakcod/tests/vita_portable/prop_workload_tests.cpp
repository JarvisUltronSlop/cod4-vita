#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_prop_workload.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <algorithm>
using namespace kisak::vita;
int main() {
    for(bool initiallyEnabled:{false,true})for(int x:{-20,0,200}) {
        bool enabled=initiallyEnabled;int box[4]={x,30,300,200};unsigned changes=0;
        GfxVitaPropScissor saved;
        const auto read=[&](bool& e,int* b){e=enabled;std::copy(box,box+4,b);};
        const auto set=[&](bool e,const int* b){enabled=e;std::copy(b,b+4,box);++changes;};
        assert(saved.Begin(read,set));assert(enabled && box[0]==0 && box[1]==0 && box[2]==1 && box[3]==1);
        assert(!saved.Begin(read,set));assert(changes==1); // A nested begin must not lose the original state.
        saved.Restore(set);assert(enabled==initiallyEnabled && box[0]==x && box[1]==30 && box[2]==300 && box[3]==200);
        saved.Restore(set);assert(changes==2); // Teardown/rejection cleanup is idempotent.
    }
    VitaRenderPassProbe probe;
    for(unsigned ms=0;ms<=120000;++ms) {
        const auto phase=VitaPropWorkloadPhase(ms);
        assert(phase==(ms<100000?ms/20000+1:0));
        probe.Begin(kVitaPropWorkload|phase,ms);
        assert(!probe.Fenced());
        assert(probe.ClipProps()==(ms>=20000 && ms<40000));
        assert(probe.SuppressProps()==(ms>=60000 && ms<80000));
        if(ms>=100000)assert(!probe.phase && !probe.workload);
    }
}
