#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_presented_frame_rate.h>
#include <cassert>
using namespace kisak::vita;
int main() {
    VitaPresentedFrameRate rate;
    assert(!rate.Observe(0,1,true).valid);
    for (uint64_t t=100000;t<=3000000;t+=100000) {
        const auto r=rate.Observe(t,1,true);
        assert(r.valid && r.timesTen==100 && r.intervalUs==100000);
        assert(r.durationUs<=1000000 && r.intervals<=10);
    }
    // A one-second render stall lowers the measured rate; no presentation
    // publications or source updates are added during the gap.
    const auto stalled=rate.Observe(4000000,1,true);
    assert(stalled.timesTen==10 && stalled.intervalUs==1000000);
    assert(rate.Observe(4000000,1,true).timesTen==10);
    assert(!rate.Observe(4000001,2,true).valid);
    assert(!rate.Observe(4100001,2,false).valid);
    assert(!rate.Observe(5000000,2,true).valid); // Loading/pause/diagnostic reset.
    assert(rate.Observe(5100000,2,true).timesTen==100);
    assert(!rate.Observe(1,2,true).valid); // Non-monotonic clock never wraps.
    // Weighted elapsed time, not the average of reciprocal frame times.
    rate.Reset();rate.Observe(0,3,true);rate.Observe(50000,3,true);
    assert(rate.Observe(200000,3,true).timesTen==100);
    // Delayed source readers do not change render-owner counting.
    rate.Reset();rate.Observe(0,4,true);
    VitaPresentedFrameRate::Result r;
    for(uint64_t i=1;i<=90;++i) r=rate.Observe(i*1000000/30,4,true);
    assert(r.timesTen==300 && r.intervals==30 && r.durationUs==1000000);
    // Fixed storage stays bounded even above its one-second capacity.
    rate.Reset();rate.Observe(0,5,true);
    for(uint64_t i=1;i<=1000;++i) r=rate.Observe(i*1000,5,true);
    assert(r.timesTen==10000 && r.intervals==255 && r.durationUs==255000);
}
