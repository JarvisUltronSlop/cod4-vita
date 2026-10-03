#ifdef NDEBUG
#undef NDEBUG
#endif
#include <gfx_vita/gfx_vita_texture_reclaim.h>
#include <cassert>
#include <array>
#include <cstddef>

// Model the actual deferred-deletion contract: deletion queues attached
// storage, whereas a detached image can be freed exactly once after completion.
struct FakeGl {
    std::array<void*,4> images{};
    uint32_t bound=3;
    bool finished=false;
    size_t live=0, deferred=0, freed=0;
    uint32_t Bound() { return bound; }
    void Bind(uint32_t id) { bound=id; }
    void* Pixels() { return images[bound]; }
    void Detach() { assert(finished);images[bound]=nullptr; }
    void Delete(uint32_t id) { if(images[id])++deferred;images[id]=nullptr; }
    void Free(void* pointer) {
        assert(finished && pointer && live==1 && !images[1]);--live;++freed;
    }
};
int main() {
    FakeGl gl;int storage=0;
    assert(!GfxVitaReclaimCompletedTexture(0,gl) && gl.bound==3);
    assert(!GfxVitaReclaimCompletedTexture(1,gl) && gl.bound==3);
    for(unsigned frame=0;frame<10;++frame) {
        for(unsigned upload=0;upload<1000;++upload) {
            gl.images[1]=&storage;gl.live=1;gl.finished=true;
            // Cover an active victim and an unrelated current-unit binding.
            gl.bound=upload%2 ? 1 : 3;
            assert(GfxVitaReclaimCompletedTexture(1,gl));
            assert(gl.bound==(upload%2 ? 0u : 3u));
            assert(!gl.live && !gl.deferred && gl.freed==frame*1000+upload+1);
            assert(!GfxVitaReclaimCompletedTexture(1,gl));
        }
    }
}
