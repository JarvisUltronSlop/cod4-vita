#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vita/vita_cpu_profile.h>
#define KISAK_VITA_CPU_RUNTIME 1
#define iassert assert
#ifndef __cdecl
#define __cdecl
#endif
using ALint=int;
using ALuint=unsigned;
constexpr int AL_PLAYING=1, AL_STOPPED=2, AL_PAUSED=3, AL_SOURCE_STATE=4,
    AL_BUFFERS_QUEUED=5, AL_BUFFER=6, SND_FIRST_STREAM_CHANNEL=40;
using snd_stopsounds_arg_t=int;
constexpr int SND_STOP_ALL=0, SND_CHANNELVOLPRIO_COUNT=5, WEAPON_READY=0;
struct AlStreamState {
    bool active=false, observedPlaying=false, underrunActive=false;
    uint64_t lastServiceUs=0;
    unsigned generation=0, bufferCount=0, availableCount=0;
} streams[13];
struct Channel {bool paused=false; int startDelay=0, alias0=0, entchannel=0;};
struct {bool Initialized2d=true; int max_2D_channels=8,max_3D_channels=32,max_stream_channels=13;
    Channel chaninfo[53];} g_snd;
struct {int source[53];} alGlob;
int states[53]{}, queued[53]{}, voices=0, residentReferences=0;
bool loadedActive[40]{}, decoderOpen[13]{}, closePending[13]{};
unsigned closes=0, drains=0, plays=0, fills=0, underruns=0, waits=0, stops=0;
unsigned failedSoundStops=0;
bool failSound=false,failClient=false,failServer=false,sourceError=false,soundErrorInjected=false;
AlStreamState* SND_StreamState(int i){return &streams[i-40];}
void alSourceStop(int i){states[i]=AL_STOPPED;++stops;}
void alGetSourcei(int i,int field,int* v){*v=field==AL_SOURCE_STATE?states[i]:queued[i];}
void alSourceUnqueueBuffers(int i,int,ALuint* v){assert(queued[i]>0);--queued[i];*v=1;}
void alSourcei(int i,int,int){assert(queued[i]==0);}
void alSourcePlay(int i){++plays;states[i]=AL_PLAYING;}
bool SND_FillStreamBuffers(int){++fills;return true;}
void SND_ResetChannelInfo(int i){auto ent=g_snd.chaninfo[i].entchannel;g_snd.chaninfo[i]={};g_snd.chaninfo[i].entchannel=ent;}
void SND_RemoveVoice(int){assert(voices>0);--voices;}
bool SND_Is2DChannelFree(int i){return !loadedActive[i];}
bool SND_Is3DChannelFree(int i){return !loadedActive[i];}
void SND_Stop2DChannel(int i){loadedActive[i]=false;--residentReferences;}
void SND_Stop3DChannel(int i){SND_Stop2DChannel(i);}
void SND_DeactivateEnvironmentEffects(int,int){}
void SND_DeactivateChannelVolumes(int,int){}
void SND_DeactivateAllEq(int){}
static void SND_ServiceExistingStreams();
void AssertReleased(){
    for(int i=0;i<13;++i){assert(!streams[i].active && !decoderOpen[i] && !closePending[i]);assert(queued[i+40]==0);}
    assert(voices==0 && residentReferences==0);
}
namespace kisak::vita {
uint64_t VitaCpuNow(){static uint64_t now=0;return now+=120000000;}
void VitaCpuAudio_Close(uint32_t i,uint32_t gen){assert(gen==streams[i].generation);closePending[i]=true;++closes;}
void VitaCpuAudio_Drain(){for(int i=0;i<13;++i){decoderOpen[i]=false;closePending[i]=false;}++drains;}
struct AudioStatus{unsigned activeStreams=0;};
AudioStatus VitaCpuAudio_ReadStatus(){AudioStatus s;for(bool open:decoderOpen)s.activeStreams+=open;return s;}
void VitaCpuAudio_ReportUnderrun(uint32_t){++underruns;}
void VitaCpuProfile_Counter(const char* name,uint64_t a,uint64_t b){
    if(!std::strcmp(name,"source-audio-stopped")){assert(b==0);if(!a)++failedSoundStops;}
}
void VitaCpuProfile_Flush(bool){SND_ServiceExistingStreams();}
void VitaCpuRender_Drain(){++waits;if(!failSound)AssertReleased();SND_ServiceExistingStreams();}
void VitaCpuFiles_Drain(){if(!failSound)AssertReleased();SND_ServiceExistingStreams();}
}
using namespace kisak::vita;
enum class VitaSourceSessionState{Cold,Initialized,Active,Paused,Failed};
struct Status{VitaSourceSessionState state=VitaSourceSessionState::Active;};
struct dvar_s{struct{bool enabled=true;}current;};
dvar_s pause,server;
const dvar_s *cl_paused=&pause,*sv_paused=&pause,*com_sv_running=&server;
void Dvar_SetInt(dvar_s*,int){}
struct{bool cgameInitCalled=true,isRunning=true;}clientUIActives[1];
struct{int hunkUsersStarted=1;}cls;
class VitaSourceSession {
public:
    bool sourceSoundInitialized_=true,clientRootInitialized_=true,serverRootInitialized_=true;
    Status status_;
    int tickAccumulatorMs_=1,clientAccumulatorMs_=1,playerPresentation_=1,matchPresentation_=1,
        grenadePresentation_=1,previousWeaponState_=1,previousWeaponShotCount_=1,initialJoinElapsedMs_=1,initialJoinStage_=1;
    bool SourceBoundarySucceeded(const char*){if(sourceError){status_.state=VitaSourceSessionState::Failed;return false;}return true;}
    void Stop();
};
void VitaScriptServicesClearError(){sourceError=false;}
template<class F> void InvokeSourceRoot(F f){try{if(failSound && !soundErrorInjected){soundErrorInjected=true;throw std::runtime_error("sound stop");}f();}catch(...){sourceError=true;}}
void CL_ShutdownCGame(int){if(failClient)throw std::runtime_error("client");clientUIActives[0].cgameInitCalled=false;}
void SV_Shutdown(const char*){if(failServer)throw std::runtime_error("server");clientUIActives[0].isRunning=false;}
void CL_Disconnect(int){clientUIActives[0].isRunning=false;}
void VitaSourceModel_Reset(){}
void VitaLoopback_Reset(){}
void VitaSourceLoopback_Reset(){}
void VitaSourcePlatform_ResetSessionState(){}
#include "source_session_stop.inc"
void Populate(unsigned generation){
    voices=13;residentReferences=40;
    for(int i=0;i<40;++i)loadedActive[i]=true;
    for(int i=0;i<13;++i){
        streams[i]={true,true,false,1,generation,4,0};decoderOpen[i]=true;
        const int ch=i+40;alGlob.source[ch]=ch;queued[ch]=4;
        states[ch]=i%3==0?AL_PLAYING:(i%3==1?AL_PAUSED:AL_STOPPED);
        g_snd.chaninfo[ch]={i%3==1,i%3==2?100:0,1,i};
    }
    clientUIActives[0]={true,true};
}
int main(){
    // Actual source StopSounds, driver release/free/service and session Stop
    // are extracted unchanged. OS/AL operations and unrelated source roots
    // are stubbed; worker generation cancellation has its own threaded test.
    for(unsigned generation=1;generation<=10;++generation){
        Populate(generation);VitaSourceSession s;
        if(generation%2==0)s.status_.state=VitaSourceSessionState::Paused;
        const auto beforeCloses=closes,beforePlays=plays,beforeFills=fills;
        s.Stop();AssertReleased();assert(s.status_.state==VitaSourceSessionState::Initialized);
        assert(closes-beforeCloses==13);
        for(int idle=0;idle<8;++idle)SND_ServiceExistingStreams();
        s.Stop();AssertReleased();assert(closes-beforeCloses==13);
        assert(plays==beforePlays && fills==beforeFills && underruns==0);
        assert(s.tickAccumulatorMs_==0 && s.clientAccumulatorMs_==0);
    }
    // Uninitialized source sound must never enter the source stop operation.
    VitaSourceSession cold;cold.sourceSoundInitialized_=false;
    cold.status_.state=VitaSourceSessionState::Cold;
    failSound=true;auto beforeFailed=failedSoundStops;cold.Stop();
    assert(cold.status_.state==VitaSourceSessionState::Cold && failedSoundStops==beforeFailed);
    // A source error cannot be overwritten by the normal Initialized reset.
    VitaSourceSession bad;bad.Stop();assert(bad.status_.state==VitaSourceSessionState::Failed);
    assert(failedSoundStops==beforeFailed+1);failSound=false;
    for(int failure=0;failure<2;++failure){
        Populate(20+failure);VitaSourceSession s;
        failClient=failure==0;failServer=failure==1;
        s.Stop();AssertReleased();assert(s.status_.state==VitaSourceSessionState::Failed);
        failClient=failServer=false;
    }
    assert(waits>20 && drains>20 && underruns==0);
}
