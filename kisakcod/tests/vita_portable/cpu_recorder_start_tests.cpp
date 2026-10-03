#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_cpu_recorder_start.h>
#include <cassert>
#include <vector>
#include <string>
#include <utility>
using namespace kisak::vita;
int main() {
    const auto valid=[](int priority) {
        return (priority>=64 && priority<=191)
            || (priority>=0x100000e0 && priority<=0x1000011f);
    };
    assert(!valid(0x10000120));
    for(int failure:{0,1,2}) {
        int starts=0,deletes=0;
        std::vector<std::pair<std::string,int>> records;
        const int result=VitaCpuStartRecorder(
            [&](int priority){assert(valid(priority));assert(priority>176);return failure==1?-23:17;},
            [&](int id){assert(id==17);++starts;return failure==2?-44:0;},
            [&](int id){assert(id==17);++deletes;},
            [&](const char* stage,int code){records.emplace_back(stage,code);});
        assert(records[0].first=="recorder-create");
        if(failure==1) {
            assert(result==-23 && starts==0 && deletes==0 && records.size()==1);
            assert(records[0].second==-23);
        } else {
            assert(starts==1 && records.size()==2 && records[0].second==17);
            assert(records[1].first=="recorder-start");
            assert(result==(failure==2?-44:17));
            assert(deletes==(failure==2?1:0));
            assert(records[1].second==(failure==2?-44:0));
        }
    }
}
