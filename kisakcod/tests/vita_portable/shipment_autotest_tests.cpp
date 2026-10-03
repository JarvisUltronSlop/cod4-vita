#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_shipment_autotest.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <initializer_list>
using Test=kisak::vita::VitaShipmentAutoTest;
using Event=Test::Event;
int main() {
    Test test;
    assert(!test.Request(false,true,0));
    assert(!test.Request(true,false,0));
    assert(test.Request(true,true,100));
    assert(!test.Request(true,true,101));
    // Loading, even longer than the entire run, never consumes play time.
    assert(test.Tick(300000100,false,false,false,false)==Event::None);
    assert(test.ElapsedUs()==0 && test.Status()==Test::State::Loading);
    assert(test.Tick(300000101,true,false,false,false)==Event::Began);
    assert(test.LoadUs()==300000001 && test.Input().forward==64);
    const uint64_t start=300000101;
    unsigned cycles=0, grenades=0, phases=0;
    for(uint64_t us=0;us<Test::kDurationUs;us+=10000) {
        test.Tick(start+us,true,false,false,false);
        cycles+=test.Input().cycle;grenades+=test.Input().fragPressed;
        phases+=test.PhaseChanged();
    }
    assert(cycles==6 && grenades==6 && phases==59 && test.SkippedPhases()==0);
    assert(test.Tick(start+Test::kDurationUs-1,true,false,false,false)==Event::None);
    assert(test.Tick(start+Test::kDurationUs,true,false,false,false)==Event::Completed);
    assert(test.ElapsedUs()==Test::kDurationUs && !test.Active());
    assert(!test.Input().fire && !test.Input().forward);
    assert(test.Tick(start+Test::kDurationUs+1,true,false,false,false)==Event::None);
    char label[128];test.Label(label,sizeof(label),true);assert(std::strstr(label,"COMPLETE"));
    test.SetLogFailure(true);test.Label(label,sizeof(label),true);assert(std::strstr(label,"LOG ERRORS"));
    assert(test.Request(true,true,900000000) && test.Run()==2);
    assert(test.Tick(900000001,false,false,true,false)==Event::Cancelled);
    assert(test.Request(true,true,900000002));test.Tick(900000003,true,false,false,false);
    test.Interrupt();
    assert(test.Tick(1100000000,true,false,false,false)==Event::Cancelled);
    assert(test.Request(true,true,1200000000));
    assert(test.Tick(1200000000,false,true,false,false)==Event::Failed);
    assert(test.Request(true,true,1300000000));
    assert(test.Tick(1299999999,false,false,false,false)==Event::Failed);
    assert(test.Request(true,true,1400000000));test.Tick(1400000001,true,false,false,false);
    assert(test.Tick(1400000002,false,false,false,false)==Event::Failed);
    // Delayed consumers account for skipped input phases instead of pretending
    // all actions ran. Only the edge of the newly reached phase is emitted.
    Test slow;slow.Request(true,true,0);slow.Tick(0,true,false,false,false);
    slow.Tick(25000000,true,false,false,false);
    assert(slow.Phase()==7 && slow.SkippedPhases()==6 && slow.Input().cycle);
    slow.Tick(25500000,true,false,false,false);assert(!slow.Input().cycle);
    assert(slow.ReportDue() && !slow.ReportDue());
    slow.Tick(25510000,true,false,false,false);assert(!slow.ReportDue());
    slow.Tick(25800000,true,false,false,false);assert(slow.ReportDue());
    assert(slow.Tick(181000000,true,false,false,false)==Event::Completed);
    assert(slow.SkippedPhases()==58);
    // Angular displacement is a function of wall time, not frame count.
    for(uint64_t interval:{10000ull,750000ull,2100000ull}) {
        Test look;look.Request(true,true,0);look.Tick(0,true,false,false,false);
        double yaw=0;
        for(uint64_t at=interval;at<12000000;at+=interval) {
            look.Tick(at,true,false,false,false);yaw+=look.Input().yawDegrees;
        }
        look.Tick(12000000,true,false,false,false);yaw+=look.Input().yawDegrees;
        assert(std::fabs(yaw-32)<0.0001);
        look.Tick(30000000,true,false,false,false);yaw+=look.Input().yawDegrees;
        assert(std::fabs(yaw)<0.0001);
    }
}
