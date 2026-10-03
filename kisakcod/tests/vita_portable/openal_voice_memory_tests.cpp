// Compiled with the exact AllocateVoices body extracted from the pinned,
// patched dependency. The fixtures model its storage contract, not the mixer.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <climits>
using ALsizei = int;
struct ALvoiceProps { int value; int Send[1]; };
struct ALvoice {
    void* Update;
    ALvoiceProps* Props;
    int value;
    char mixerStorage[33000];
    int Send[1];
};
struct ALCdevice { int NumAuxSends; };
struct ALCcontext { ALCdevice* Device; ALvoice** Voices; int MaxVoices; int VoiceCount; };
static bool failAllocation = false;
static size_t requested = 0, live = 0, freed = 0, deinitialized = 0;
static ALvoice** al_calloc(size_t, size_t size) {
    requested = size;
    if (failAllocation || size > 32*1024*1024) return nullptr;
    auto* result = static_cast<ALvoice**>(std::calloc(1, size));
    if (result) ++live;
    return result;
}
static void al_free(void* p) { if (p) { --live; ++freed; std::free(p); } }
static void DeinitVoice(ALvoice*) { ++deinitialized; }
static int mini(int a, int b) { return a < b ? a : b; }
static size_t RoundUp(size_t value, size_t alignment) { return (value+alignment-1)&~(alignment-1); }
#define FAM_SIZE(T, member, count) (offsetof(T, member)+sizeof(((T*)nullptr)->member[0])*(count))
#define ATOMIC_INIT(p, value) (*(p)=(value))
#include "openal_voice_function.inc"

int main() {
    for (int cycle=0; cycle<20; ++cycle) {
        ALCdevice device{2}; ALCcontext context{&device,nullptr,0,0};
        failAllocation=true;
        assert(!AllocateVoices(&context,64,2));
        assert(!context.Voices && !context.MaxVoices && live==0);
        failAllocation=false;
        assert(AllocateVoices(&context,64,2));
        assert(context.MaxVoices==64 && live==1);
        const auto initialSize=requested;
        context.VoiceCount=53;
        for (int i=0;i<53;++i) {
            context.Voices[i]->value=i+10;
            context.Voices[i]->Props->value=i+20;
            for (int s=0;s<2;++s) {
                context.Voices[i]->Send[s]=i+s;
                context.Voices[i]->Props->Send[s]=i+s+100;
            }
        }
        auto* old=context.Voices; const auto oldFreed=freed;
        failAllocation=true;
        // A failed growth must leave every active voice and pointer intact.
        assert(!AllocateVoices(&context,128,2));
        assert(context.Voices==old && context.MaxVoices==64 && context.VoiceCount==53);
        assert(freed==oldFreed && live==1);
        // An unchanged capacity must not allocate at all.
        assert(AllocateVoices(&context,64,2) && context.Voices==old);
        device.NumAuxSends=4;
        assert(!AllocateVoices(&context,64,2));
        assert(context.Voices==old && freed==oldFreed);
        failAllocation=false;
        assert(AllocateVoices(&context,128,2));
        assert(context.MaxVoices==128 && context.VoiceCount==53 && live==1);
        for (int i=0;i<53;++i) {
            assert(context.Voices[i]->value==i+10 && context.Voices[i]->Props->value==i+20);
            for (int s=0;s<2;++s) {
                assert(context.Voices[i]->Send[s]==i+s);
                assert(context.Voices[i]->Props->Send[s]==i+s+100);
            }
        }
        assert(!AllocateVoices(&context,INT_MAX,4));
        assert(context.MaxVoices==128 && live==1);
        assert(AllocateVoices(&context,32,4));
        assert(context.VoiceCount==32 && deinitialized==size_t(cycle+1)*21);
        al_free(context.Voices);
        assert(live==0);
        if (cycle==19) std::printf("20 voice-pool lifetimes passed; initial fixture allocation=%zu bytes\n",initialSize);
    }
}
