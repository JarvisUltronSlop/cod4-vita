#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <vita/vita_cpu_profile.h>
#define VITA_CPU_SCOPE_TEST_UNUSED 1
using ALint=int;
constexpr int AL_PLAYING=1,AL_STOPPED=2,AL_PAUSED=3,AL_SOURCE_STATE=4,AL_BUFFERS_QUEUED=5;
constexpr int SND_FIRST_STREAM_CHANNEL=40;
struct Stream {bool active=false,observedPlaying=false,underrunActive=false;uint64_t lastServiceUs=0;};
Stream streams[13];
struct {int max_stream_channels=13;struct {bool paused=false;int startDelay=0;} chaninfo[53];} g_snd;
struct {int source[53];} alGlob;
int states[53],queued[53],fills[53],plays[53];bool more[53];
unsigned underruns=0,gaps=0;
Stream* SND_StreamState(int i){assert(i>=40 && i<53);return &streams[i-40];}
bool SND_FillStreamBuffers(int i){++fills[i];return more[i];}
void alGetSourcei(int i,int field,int* v){*v=field==AL_SOURCE_STATE?states[i]:queued[i];}
void alSourcePlay(int i){++plays[i];states[i]=AL_PLAYING;}
namespace kisak::vita {
uint64_t VitaCpuNow(){static uint64_t now=0;return now+=20000;}
void VitaCpuAudio_ReportUnderrun(uint32_t){++underruns;}
void VitaCpuProfile_Counter(const char*,uint64_t,uint64_t){++gaps;}
}
#include "source_audio_service.inc"
int main() {
    for(int i=40;i<53;++i){alGlob.source[i]=i;states[i]=AL_PLAYING;queued[i]=4;more[i]=true;streams[i-40].active=true;}
    streams[0].active=false;g_snd.chaninfo[41].paused=true;g_snd.chaninfo[42].startDelay=1;
    states[43]=AL_PAUSED; // preserve a paused AL source even if source flag differs
    states[44]=AL_STOPPED;more[44]=false;queued[44]=0; // natural EOF
    states[45]=AL_STOPPED;queued[45]=0; // decoder has not published first block
    SND_ServiceExistingStreams();
    for(int i=40;i<43;++i)assert(fills[i]==0);
    for(int i=43;i<53;++i)assert(fills[i]==1);
    for(int i=40;i<53;++i)assert(plays[i]==0);
    assert(underruns==0 && states[43]==AL_PAUSED);
    // Previously playing stream empties, remains empty, then gets new PCM.
    states[46]=AL_STOPPED;queued[46]=0;
    SND_ServiceExistingStreams();assert(underruns==1 && plays[46]==0);
    SND_ServiceExistingStreams();assert(underruns==1);
    queued[46]=4;SND_ServiceExistingStreams();assert(underruns==1 && plays[46]==1);
    SND_ServiceExistingStreams(); // observes the resumed playing state
    states[46]=AL_STOPPED;SND_ServiceExistingStreams();assert(underruns==2 && plays[46]==2 && gaps==2);
    // Cancelled/replaced streams cannot inherit the old playback state.
    streams[6]={};SND_ServiceExistingStreams();assert(underruns==2);
    streams[6].active=true;states[46]=AL_STOPPED;
    SND_ServiceExistingStreams();assert(underruns==2 && plays[46]==3);
    // Service never mutates event state, delayed-start clocks or pause flags.
    assert(g_snd.chaninfo[41].paused && g_snd.chaninfo[42].startDelay==1);
}
