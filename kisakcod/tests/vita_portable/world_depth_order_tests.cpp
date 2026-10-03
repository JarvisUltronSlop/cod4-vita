#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_world_depth_order.h>
#include <vita/vita_render_pass_probe.h>
#include <cassert>
#include <vector>
#include <numeric>
#include <random>
#include <limits>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
using namespace kisak::vita;
int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    VitaDepthScratch scratch;
    assert(!scratch.Reserve(4,+[](size_t)->VitaDepthRange*{return nullptr;}));assert(!scratch.data() && !scratch.size());
    assert(scratch.Reserve(4));auto* original=scratch.data();scratch[0]={.1f,.2f,true};
    assert(scratch.Reserve(2) && scratch.data()==original);
    assert(!scratch.Reserve(8,+[](size_t)->VitaDepthRange*{return nullptr;}) && scratch.data()==original && scratch[0].valid);
    assert(!scratch.Reserve(SIZE_MAX));scratch.Clear();assert(!scratch.data() && !scratch.size());
    const uint32_t state0=0x18000800u;
    for(uint32_t state1=0;state1<65536;++state1)
        assert(VitaDepthOrderState(true,true,state0,state1)==(state1==5 || state1==13));
    for(uint32_t bit:{0x100u,0x01000000u,0x07000000u})assert(!VitaDepthOrderState(true,true,state0|bit,13));
    for(uint32_t bit:{0x800u,0x08000000u,0x10000000u})assert(!VitaDepthOrderState(true,true,state0&~bit,13));
    assert(!VitaDepthOrderState(false,true,state0,13) && !VitaDepthOrderState(true,false,state0,13));
    float matrix[16]{};matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;
    float lo[3]={-1,-1,-.5f},hi[3]={1,1,.5f};
    auto range=VitaProjectDepthRange(matrix,lo,hi);assert(range.valid && range.nearDepth==-.5f && range.farDepth==.5f);
    matrix[15]=-1;assert(!VitaProjectDepthRange(matrix,lo,hi).valid);matrix[15]=1;
    hi[2]=1;assert(!VitaProjectDepthRange(matrix,lo,hi).valid);hi[2]=.5f;
    matrix[10]=std::numeric_limits<float>::quiet_NaN();assert(!VitaProjectDepthRange(matrix,lo,hi).valid);
    std::mt19937 rng(36);
    for(unsigned test=0;test<1000;++test) {
        const unsigned n=2+rng()%90;std::vector<VitaDepthRange> ranges(n);std::vector<uint32_t> order(n);std::iota(order.begin(),order.end(),0);
        for(auto& v:ranges){v.nearDepth=(int(rng()%100)-50)*.01f;v.farDepth=v.nearDepth+(rng()%20)*.01f;v.valid=(rng()%8)!=0;}
        const auto before=VitaDepthMembership(order.data(),n);auto stats=VitaOrderWorldDepth(order.data(),n,ranges.data());
        assert(stats.comparisons<=kVitaDepthOrderComparisonLimit && stats.swaps<=stats.comparisons);
        assert(before==VitaDepthMembership(order.data(),n));
        std::vector<unsigned> positions(n),seen(n);
        for(unsigned i=0;i<n;++i){assert(order[i]<n && !seen[order[i]]++);positions[order[i]]=i;}
        for(unsigned a=0;a<n;++a)for(unsigned b=a+1;b<n;++b)if(positions[a]>positions[b])
            assert(ranges[a].valid && ranges[b].valid && ranges[b].farDepth+.0001f<ranges[a].nearDepth);
        // Independent opaque pixel oracle: original and reordered streams must
        // resolve the same winning surface for random covered fragments.
        for(unsigned pixel=0;pixel<20;++pixel) {
            std::vector<float> z(n);for(unsigned i=0;i<n;++i)z[i]=rng()%3?2.0f:ranges[i].nearDepth+(ranges[i].farDepth-ranges[i].nearDepth)*(rng()%101)/100.f;
            const auto draw=[&](bool sorted){float depth=3;unsigned color=~0u;for(unsigned i=0;i<n;++i){const unsigned id=sorted?order[i]:i;if(z[id]<1 && z[id]<=depth){depth=z[id];color=id;}}return color;};
            assert(draw(false)==draw(true));
        }
    }
    std::vector<uint32_t> order(4096);std::iota(order.begin(),order.end(),0);
    std::vector<VitaDepthRange> ranges(4096);for(unsigned i=0;i<4096;++i)ranges[i]={1-i*.0004f,1-i*.0004f,true};
    auto stats=VitaOrderWorldDepth(order.data(),order.size(),ranges.data());assert(stats.swaps>0 && stats.comparisons==kVitaDepthOrderComparisonLimit);
    for(unsigned phase:{2049u,2050u,2051u}){VitaRenderPassProbe probe;probe.Begin(phase,0);assert(probe.depthOrder && !probe.Fenced());}
}
