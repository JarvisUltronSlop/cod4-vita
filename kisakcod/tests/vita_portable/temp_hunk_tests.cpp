#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_temp_hunk.h>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstring>

namespace {
std::jmp_buf boundary;
const char* failureReason = nullptr;
size_t calls = 0, failAt = 0, live = 0;
void* Allocate(size_t count, size_t bytes) {
    ++calls;
    if (failAt && calls == failAt) return nullptr;
    void* p = std::calloc(count, bytes);
    assert(p); ++live; return p;
}
void Release(void* p) { assert(p && live); --live; std::free(p); }
void Fail(const char* reason) {
    failureReason = reason;
    std::longjmp(boundary, 1);
}
kisak::vita::VitaTempHunk<4> arena(Allocate, Release);
}

int main() {
    // Inject heap failure at every position and model the real source error
    // boundary. Earlier allocations survive until explicit session cleanup.
    for (size_t position = 1; position <= 4; ++position) {
        calls = 0; failAt = position; failureReason = nullptr;
        if (setjmp(boundary) == 0) {
            for (int i = 0; i < 4; ++i) arena.Allocate(536, Fail);
            assert(false);
        }
        assert(std::strcmp(failureReason, "heap exhausted") == 0);
        assert(arena.Count() == position - 1 && live == position - 1);
        arena.Clear(); arena.Clear(); assert(!live && !arena.Count());
    }
    failAt = 0;
    // Repeated load/clear cycles, source alignment/zeroing and stable pointers.
    for (int cycle = 0; cycle < 50; ++cycle) {
        auto* first = static_cast<unsigned char*>(arena.Allocate(536, Fail));
        assert(reinterpret_cast<uintptr_t>(first) % alignof(std::max_align_t) == 0);
        for (int i = 0; i < 536; ++i) assert(first[i] == 0);
        std::memset(first, 0x5a, 536);
        auto* second = arena.Allocate(4096, Fail);
        arena.Allocate(32, Fail); arena.Allocate(472, Fail);
        assert(arena.Count() == 4);
        const size_t before = calls;
        if (setjmp(boundary) == 0) { arena.Allocate(1, Fail); assert(false); }
        assert(std::strcmp(failureReason, "allocation table full") == 0);
        assert(calls == before && live == 4); // No freed pointer escapes.
        for (int i = 0; i < 536; ++i) assert(first[i] == 0x5a);
        assert(arena.Untrack(second)); assert(!arena.Untrack(second));
        Release(second);
        arena.Allocate(64, Fail); assert(live == 4);
        arena.Clear(); assert(!arena.Count() && !live);
    }
    if (setjmp(boundary) == 0) { arena.Allocate(0, Fail); assert(false); }
    assert(std::strcmp(failureReason, "invalid size") == 0 && !live);
    if (setjmp(boundary) == 0) { arena.Allocate(-1, Fail); assert(false); }
    assert(std::strcmp(failureReason, "invalid size") == 0 && !live);
    arena.Allocate(4, Fail); arena.Clear(); assert(!live);
}
