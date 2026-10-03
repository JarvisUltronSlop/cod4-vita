#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vita/vita_cpu_tls_probe.h>

int main() {
    unsigned checks=0;
    const auto report=[&](const char* stage,int result,int){
        if(stage[4]=='i'){assert(result==0);++checks;}
    };
    for(unsigned i=0;i<24;++i) {
        assert(kisak::vita::VitaCpuVerifyThreadLocals(report));
        assert(CpuTestLiveThreads()==0);
    }
    assert(checks==48);
    CpuTestFailNextThreadStart();
    assert(!kisak::vita::VitaCpuVerifyThreadLocals([](const char*,int,int){}));
    assert(CpuTestLiveThreads()==0);
    assert(kisak::vita::VitaCpuVerifyThreadLocals(report));
}
