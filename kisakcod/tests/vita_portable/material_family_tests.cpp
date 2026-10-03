#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_material_family.h>
#include <vita/vita_render_pass_probe.h>
#include <vita/vita_gpu_diagnostic_anchor.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <limits>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
int main(int argc,char** argv) {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file);
    std::string text((std::istreambuf_iterator<char>(file)),{});
    for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
    auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);
    a=text.find('(',a)+1;auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);
    const auto original=text.substr(a,b-a);
    for(unsigned bits=0;bits<1024;++bits) {
        bool v[10];for(unsigned i=0;i<10;++i)v[i]=(bits&(1u<<i))!=0;
        const auto actual=GfxVitaMaterialFamily(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9]);
        const bool common=v[0] && v[1] && !v[2] && v[3] && v[4] && v[5];
        const unsigned expected=!common?0u:(v[6] && v[7] && v[8])?1u:(!v[6] && !v[7] && !v[8] && v[9])?2u:0u;
        assert(actual==expected);
    }
    const char* names[]={"u_has_color","u_has_normal","u_has_specular","u_has_reflection_probe","u_use_primary_lightmap","u_use_secondary_lightmap","u_baked_lighting"};
    std::string baseline;assert(GfxVitaBuildOpaqueShader(original.c_str(),true,baseline));
    for(unsigned family=1;family<=2;++family) {
        std::string candidate;assert(GfxVitaBuildMaterialFamily(original.c_str(),family,candidate));
        assert(!GfxVitaShaderHasDiscard(candidate));
        for(unsigned i=0;i<(family==2?7u:6u);++i) {
            const std::string uniform=std::string("uniform float ")+names[i]+";";
            const std::string constant=std::string("const float ")+names[i]+" = "+(i<3 || i==6 || family==1?"1.0;":"0.0;");
            const auto at=candidate.find(constant);assert(at!=std::string::npos);
            candidate.replace(at,constant.size(),uniform);
            auto broken=original;const auto location=broken.find(uniform);assert(location!=std::string::npos);
            broken.erase(location,uniform.size());std::string unused;
            assert(!GfxVitaBuildMaterialFamily(broken.c_str(),family,unused));
            assert(!GfxVitaBuildMaterialFamily((original+"\n"+uniform).c_str(),family,unused));
        }
        assert(candidate==baseline); // no equations, samplers or precision changed
    }
    std::string out;assert(!GfxVitaBuildMaterialFamily(original.c_str(),0,out));assert(!GfxVitaBuildMaterialFamily(original.c_str(),3,out));
    for(unsigned elapsed=0;elapsed<=80000;++elapsed) {
        const auto phase=kisak::vita::VitaRenderProbePhase(elapsed);kisak::vita::VitaRenderPassProbe probe;
        probe.Begin(kisak::vita::kVitaMaterialFamily+phase,0);
        assert(probe.family==(phase!=0));assert(!probe.Fenced() && !probe.ClipProps() && !probe.SuppressProps());
        assert(!probe.material && !probe.specular && !probe.baseline && !probe.depthOrder);
    }
    float anchor[]={158,735,196},angles[]={0,90,0},server[]={158,735,195.35f},client[]={158,735,195.35f},velocity[]={0,0,0};
    const auto ready=[&]{return kisak::vita::VitaGpuAnchorReady(anchor,angles,server,client,angles,angles,velocity,velocity);};
    assert(ready());client[0]+=1;assert(!ready());client[0]-=1;
    server[0]+=1;client[0]+=1;assert(!ready());server[0]-=1;client[0]-=1;
    velocity[2]=-1;assert(!ready());velocity[2]=0;
    server[2]-=70;client[2]-=70;assert(!ready());server[2]+=70;client[2]+=70;
    float clientAngles[]={0,-270,0};assert(kisak::vita::VitaGpuAnchorReady(anchor,angles,server,client,angles,clientAngles,velocity,velocity));
    clientAngles[1]+=1;assert(!kisak::vita::VitaGpuAnchorReady(anchor,angles,server,client,angles,clientAngles,velocity,velocity));
    client[0]=std::numeric_limits<float>::quiet_NaN();assert(!ready());client[0]=158;
    velocity[0]=std::numeric_limits<float>::infinity();assert(!ready());
}
