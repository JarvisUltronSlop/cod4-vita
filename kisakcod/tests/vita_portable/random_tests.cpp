#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_crt_random.h>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <thread>

int main() {
    using namespace kisak::vita;
    VitaCrtRandom random;
    // Published Windows CRT sequence for the default seed, independently
    // checked against the locally installed Microsoft runtime below.
    constexpr std::array expected{41, 18467, 6334, 26500, 19169,
        15724, 11478, 29358, 26962, 24464};
    for (int value : expected) assert(random.Next() == value);
    for (uint32_t seed : {0u, 1u, 42u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        random.Seed(seed);
#if defined(_MSC_VER)
        std::srand(seed);
#endif
        for (unsigned i = 0; i < 1000000; ++i) {
            const int value = random.Next();
            assert(value >= 0 && value < 32768);
#if defined(_MSC_VER)
            assert(value == std::rand());
#endif
            // Original FX and random()/crandom() scaling must stay in range.
            assert(479 * value / 32768 < 479);
            assert(value / 32768.0 < 1.0);
        }
    }
    SourceCrtSrand(1);
    assert(SourceCrtRand() == expected[0]);
    std::thread worker([] {
        assert(SourceCrtRand() == 41);
        SourceCrtSrand(42);
        for (int i = 0; i < 1000; ++i) SourceCrtRand();
    });
    worker.join();
    assert(SourceCrtRand() == expected[1]);
    SourceCrtSrand(1);
    assert(SourceCrtRand() == expected[0]);
    std::puts("source CRT random: Windows sequence, range and thread isolation passed");
}
