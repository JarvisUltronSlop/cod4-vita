#include <vita/vita_hunk_arena.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using kisak::vita::VitaHunkArena;
static void Check(bool ok) { if (!ok) std::abort(); }
static size_t outstanding = 0;
static bool deny = false;
static void* Allocate(size_t bytes) {
    if (deny) return nullptr;
    void* result = std::malloc(bytes);
    if (result) ++outstanding;
    return result;
}
static void Release(void* pointer) { if (pointer) --outstanding; std::free(pointer); }

int main() {
    for (int cycle = 0; cycle < 10; ++cycle) {
        VitaHunkArena arena(Allocate, Release);
        Check(arena.Reserved() == 0);
        auto* retained = static_cast<uint8_t*>(arena.Allocate(100, 4096));
        Check(retained && reinterpret_cast<uintptr_t>(retained) % 4096 == 0);
        Check(arena.Reserved() == VitaHunkArena::kPageBytes);
        Check(retained[99] == 0);
        std::memset(retained, 0x5a, 100);
        const size_t mark = arena.Mark();
        auto* transient = static_cast<uint8_t*>(arena.Allocate(800000, 32));
        Check(transient && transient[799999] == 0 && arena.Contains(transient));
        Check(retained[0] == 0x5a && retained[99] == 0x5a);
        Check(!arena.Rewind(arena.Mark() + 1));
        Check(arena.Rewind(mark) && outstanding == 1);
        Check(!arena.Contains(transient) && arena.Contains(retained));
        Check(retained[0] == 0x5a && retained[99] == 0x5a);
        deny = true;
        Check(!arena.Allocate(800000, 32) && arena.Mark() == mark);
        Check(arena.Reserved() == VitaHunkArena::kPageBytes);
        deny = false;
        Check(arena.Allocate(800000, 32));
        Check(!arena.Allocate(SIZE_MAX, 4));
        Check(!arena.Allocate(1, 0) && !arena.Allocate(1, 3));
        Check(!arena.Allocate(1, 8192));
        arena.Clear();
        Check(outstanding == 0 && arena.Mark() == 0 && arena.Reserved() == 0);
        Check(!arena.Contains(retained));
    }
    {
        VitaHunkArena arena(Allocate, Release);
        std::vector<uint8_t*> pointers;
        while (void* pointer = arena.Allocate(60000, 64)) {
            Check(arena.Reserved() <= VitaHunkArena::kCapacity);
            auto* bytes = static_cast<uint8_t*>(pointer);
            std::memset(bytes, static_cast<int>(pointers.size() & 255), 60000);
            pointers.push_back(bytes);
        }
        Check(arena.Reserved() == VitaHunkArena::kCapacity);
        for (size_t i = 0; i < pointers.size(); ++i)
            Check(pointers[i][0] == (i & 255) && pointers[i][59999] == (i & 255));
        Check(arena.PeakReserved() == VitaHunkArena::kCapacity);
    }
    Check(outstanding == 0);
    std::puts("source hunk: stable pointers, marks, alignment, failure recovery and bounded residency passed");
}
