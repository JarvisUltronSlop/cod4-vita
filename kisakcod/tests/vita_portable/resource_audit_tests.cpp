#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iterator>
#include <regex>
#include <vector>
#include <gfx_vita/gfx_vita_resource_audit.h>
static void Put(std::vector<uint8_t>& v,size_t at,uint32_t n) {for(unsigned j=0;j<4;++j)v[at+j]=uint8_t(n>>(j*8));}
int main(int argc,char** argv) {
    assert(argc==2);std::ifstream f(argv[1]);assert(f);
    const std::string source((std::istreambuf_iterator<char>(f)),{});
    const std::regex hex("0x([0-9a-fA-F]{2})");std::vector<uint8_t> fixture;
    for(std::sregex_iterator i(source.begin(),source.end(),hex),end;i!=end;++i)fixture.push_back(uint8_t(std::stoul((*i)[1],nullptr,16)));
    GfxVitaGxpAudit a{};assert(GfxVitaParseGxp(fixture.data(),fixture.size(),a));
    assert(a.size==234 && a.primaryRegisters==2 && a.secondaryRegisters==4 && a.temporaryRegisters==0);
    assert(a.primaryInstructions==2 && a.secondaryInstructions==0 && a.primaryOffset==180 && a.compilerVersion==0x33dc0);
    const auto declaredSize=a.size;
    for(size_t n=0;n<declaredSize;++n)assert(!GfxVitaParseGxp(fixture.data(),n,a));
    assert(!GfxVitaParseGxp(nullptr,999,a));
    for(auto field:{8u,60u,64u,68u,72u}) {
        auto bad=fixture;Put(bad,field,0xffffffffu);
        if(field==72)Put(bad,68,2);
        assert(!GfxVitaParseGxp(bad.data(),bad.size(),a) && !a.bytes);
    }
    auto unknown=fixture;unknown[4]=2;assert(!GfxVitaParseGxp(unknown.data(),unknown.size(),a));
    std::vector<uint8_t> wrapped(132,0); // pinned 16 attributes, eight bytes each
    const auto append=[&](bool fragment,bool duplicate=false) {
        const size_t at=wrapped.size(),len=4+fixture.size()*(duplicate?2:1);
        wrapped.resize(at+4+len);Put(wrapped,at,uint32_t(len));Put(wrapped,at+4,0);
        auto shader=fixture;shader[20]=fragment?1:0;
        std::memcpy(wrapped.data()+at+8,shader.data(),shader.size());
        if(duplicate)std::memcpy(wrapped.data()+at+8+shader.size(),shader.data(),shader.size());
    };
    append(false);append(true);GfxVitaGxpAudit pair[2]{};
    assert(GfxVitaParseProgramAudit(wrapped.data(),wrapped.size(),132,pair));
    for(size_t n=0;n<wrapped.size();++n)assert(!GfxVitaParseProgramAudit(wrapped.data(),n,132,pair));
    wrapped.push_back(0);assert(!GfxVitaParseProgramAudit(wrapped.data(),wrapped.size(),132,pair));
    wrapped.resize(132);append(false,true);append(true);assert(!GfxVitaParseProgramAudit(wrapped.data(),wrapped.size(),132,pair));
    wrapped.resize(132);append(true);append(false);assert(!GfxVitaParseProgramAudit(wrapped.data(),wrapped.size(),132,pair));
    GfxVitaResourceAuditGate gate;
    assert(!gate.Take(1,0,261,true) && !gate.Take(1,4,261,true) && !gate.Take(1,1,500,true) && !gate.Take(1,1,261,false));
    for(unsigned phase=1;phase<=3;++phase)for(unsigned target:{261u,507u,588u}) {
        assert(gate.Take(1,phase,target,true));assert(!gate.Take(1,phase,target,true));
    }
    assert(gate.seen==511 && gate.Take(2,1,261,true) && gate.seen==1);
}
