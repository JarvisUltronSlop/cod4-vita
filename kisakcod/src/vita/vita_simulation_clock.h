#pragma once
#include <cstdint>

// Rendering may vary its rate without truncating sub-millisecond game time.
// Only explicit lifecycle boundaries reset this clock; hitches are elapsed time.
class VitaSimulationClock {
public:
    void Reset(uint64_t now) { previous_ = now; remainder_ = 0; discontinuity_ = false; }
    // Suspend/menu time is excluded without losing a running fractional ms.
    void Rebase(uint64_t now) { previous_ = now; discontinuity_ = false; }
    uint64_t Advance(uint64_t now) {
        if (now < previous_) { discontinuity_ = true; return 0; }
        const uint64_t delta = now - previous_;
        previous_ = now;
        const uint64_t total = delta + remainder_;
        remainder_ = total % 1000;
        return total / 1000;
    }
    bool Discontinuity() const { return discontinuity_; }
private:
    uint64_t previous_ = 0;
    uint64_t remainder_ = 0;
    bool discontinuity_ = false;
};
