#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_brush_scan_timing.h>
#include <cassert>
#include <string>
#include <vector>
#include <cstdio>

struct Sink {
    struct Record { std::string name; uint64_t a, b; };
    static inline uint64_t now = 100, calls = 0;
    static inline std::vector<Record> records;
    static uint64_t Now() { ++calls; return now++; }
    static void Counter(const char* name, uint64_t a, uint64_t b) { records.push_back({name,a,b}); }
};
int main() {
    kisak::vita::BrushScanSelector selector;
    for (uint32_t generation : {0u,1u,7u}) {
        for (unsigned i=0; i!=192; ++i) assert(selector.Select(generation)==(i%64==0));
    }
    kisak::vita::BrushScanSelector otherThread;
    assert(otherThread.Select(7));
    { // Unselected path must not call clocks or recorder, even with work.
        kisak::vita::BrushScanTiming<Sink> sample(false);
        sample.SkipGroup(16); sample.Visit(); sample.Reject(); sample.Visit(); sample.BeginNarrow(); sample.EndNarrow();
    }
    assert(Sink::calls==0 && Sink::records.empty());
    {
        kisak::vita::BrushScanTiming<Sink> sample(true);
        sample.Visit(); sample.Reject();
        sample.Visit(); // Content-mask exclusion: not rejected by bounds.
        sample.Visit(); sample.BeginNarrow(); Sink::now+=20; sample.EndNarrow();
        sample.Visit(); sample.BeginNarrow(); Sink::now+=30; sample.EndNarrow();
    }
    assert(Sink::calls==8 && Sink::records.size()==4);
    assert(Sink::records[0].name=="brush-sample-scan-us" && Sink::records[0].a==55 && Sink::records[0].b==52);
    assert(Sink::records[1].a==4 && Sink::records[1].b==2);
    assert(Sink::records[2].a==1 && Sink::records[2].b==1);
    assert(Sink::records[3].a==1 && Sink::records[3].b==8);
    Sink::records.clear();
    { kisak::vita::BrushScanTiming<Sink> empty(true); }
    assert(Sink::records[0].b==0 && Sink::records[1].a==0 && Sink::records[2].b==0);
    Sink::records.clear();
    { kisak::vita::BrushScanTiming<Sink> sample(true);sample.SkipGroup(16);sample.SkipGroup(3);sample.Visit();sample.Reject(); }
    assert(Sink::records.size()==5 && Sink::records[1].a==1 && Sink::records[2].a==1);
    assert(Sink::records[3].name=="brush-sample-group-skipped" && Sink::records[3].a==19 && Sink::records[3].b==2);
    std::puts("Brush sampling cadence, generation reset, disabled overhead and timing/count accounting passed");
}
