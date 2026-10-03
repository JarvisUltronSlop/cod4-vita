#include <vita/vita_simulation_clock.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void Check(bool ok) { if (!ok) std::exit(1); }
int main() {
    for (unsigned fps : {15u, 30u, 60u, 144u}) {
        VitaSimulationClock clock;
        clock.Reset(500);
        uint64_t elapsed = 0;
        for (uint64_t frame = 1; frame <= fps * 180; ++frame)
            elapsed += clock.Advance(500 + frame * 1000000 / fps);
        Check(elapsed == 180000);
    }
    VitaSimulationClock clock;
    clock.Reset(0);
    Check(clock.Advance(33333) == 33);
    Check(clock.Advance(66666) == 33);
    Check(clock.Advance(766666) == 700); // hitches remain game time
    Check(clock.Advance(767000) == 1); // fractional time is retained
    clock.Reset(10000000); // explicit loading/suspend boundary
    Check(clock.Advance(10033333) == 33);
    Check(clock.Advance(2) == 0 && clock.Discontinuity());
    clock.Reset(0);
    Check(clock.Advance(999)==0);
    clock.Rebase(5000000);
    Check(clock.Advance(5000001)==1 && !clock.Discontinuity());
    std::puts("simulation clock: rate-independent elapsed time passed");
}
