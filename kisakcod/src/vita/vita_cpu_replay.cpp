#include <vita/vita_cpu_replay.h>
#if KISAK_VITA_CPU_PROFILE
#include <vita/vita_cpu_profile.h>
#include <vita/vita_crt_random.h>
#include <vita/vita_cpu_replay_tape.h>
#include <vita/vita_input.h>
#include <bgame/bg_local.h>
#include <qcommon/msg_mp.h>
#include <game_mp/g_main_mp.h>
#include <client_mp/client_mp.h>
#include <qcommon/qcommon.h>
#include <universal/com_math.h>
#include <cstdio>
#include <cstring>

extern uint32_t VitaCpuGameplayRandomState();
namespace kisak::vita {
namespace {
bool enabled=false;
uint32_t eventOrder=0;
uint32_t capture=0;
thread_local bool tapeActive=false;
bool tapeAttempted=false;
uint32_t sourceInitializations=0;
uint8_t tapeStorage[VitaCpuReplayTape::kMaximumBytes];
VitaCpuReplayTape tape;
VitaCpuReplayTape::Input tapeInput;
uint32_t inputReads=0;
uint64_t Hash(const void* input,size_t bytes,uint64_t hash=14695981039346656037ull) {
    const auto* data=static_cast<const uint8_t*>(input);
    for(size_t i=0;i<bytes;++i){hash^=data[i];hash*=1099511628211ull;}
    return hash;
}
void Write(const char* text,int size,size_t capacity) {
    if(size<=0 || size>=int(capacity) || text[0]!='{'){VitaCpuProfile_Invalidate("replay-record-overflow");return;}
    const auto context=VitaCpuProfile_Context();
    char record[1408];const int count=std::snprintf(record,sizeof(record),
        "{\"atUs\":%llu,\"generation\":%u,\"sequence\":%u,\"capture\":%u,%s",
        (unsigned long long)VitaCpuNow(),context.generation,context.sequence,capture,text+1);
    if(count>0 && count<int(sizeof(record)))VitaCpuProfile_AppendLog(VitaCpuLog::Replay,record,uint32_t(count));
    else VitaCpuProfile_Invalidate("replay-record-overflow");
}
}
void VitaCpuReplay_Enable(bool value,bool completed){
    if(value==enabled)return;
    if(value){++capture;eventOrder=0;}
    char record[512];const int size=std::snprintf(record,sizeof(record),
        "{\"type\":\"%s\",\"schema\":2,\"stimulus\":\"%s\",\"complete\":%s,\"crtRng\":%u,\"gameRng\":%u,\"eventOrder\":%u,\"tapeCrc\":%u,\"tapeRecords\":%u,\"tapeStep\":%u,\"inputReads\":%u}\n",
        value?"begin":"end",tapeActive?"source-input-tape-v1":"cpu-movement-combat-v1",
        !value && completed?"true":"false",sourceCrtRandom.State(),VitaCpuGameplayRandomState(),eventOrder,
        tapeActive?tape.Checksum():0,tapeActive?tape.Count():0,tapeActive?tape.Index():0,inputReads);
    Write(record,size,sizeof(record));enabled=value;
}
bool VitaCpuReplay_Enabled(){return enabled;}
bool VitaCpuReplay_TapeActive(){return tapeActive;}
bool VitaCpuReplay_TapeComplete(){return tapeActive && tape.Complete();}
bool VitaCpuReplay_BeginTape() {
    if(tapeAttempted || sourceInitializations || enabled || VitaCpuCurrentRole()!=VitaCpuRole::Source)return false;
    tapeAttempted=true;
    FILE* file=std::fopen("app0:cpu-source-replay.bin","rb");
    if(!file){VitaCpuProfile_Invalidate("replay-tape-missing");return false;}
    const auto bytes=std::fread(tapeStorage,1,sizeof(tapeStorage),file);
    const bool readOk=!std::ferror(file) && std::fgetc(file)==EOF;
    const bool closeOk=std::fclose(file)==0;
    if(!readOk || !closeOk || !tape.Load(tapeStorage,bytes)) {
        VitaCpuProfile_Invalidate("replay-tape-invalid");return false;
    }
    tapeInput={};inputReads=0;tapeActive=true;
    sourceCrtRandom.Seed(tape.Seed());Rand_Init(int(tape.Seed()));
    VitaCpuReplay_Enable(true);
    VitaCpuProfile_Counter("source-replay-begin",tape.Count(),tape.Checksum());return true;
}
bool VitaCpuReplay_Clock(uint32_t* milliseconds) {
    if(!tapeActive || !milliseconds || VitaCpuCurrentRole()!=VitaCpuRole::Source)return false;
    *milliseconds=tape.Clock();return true;
}
bool VitaCpuReplay_NextFrame(uint32_t& elapsedMs) {
    if(!tapeActive || !tape.Next(tapeInput,elapsedMs))return false;
    char line[384];const int size=std::snprintf(line,sizeof(line),
        "{\"type\":\"replay-step\",\"step\":%u,\"time\":%u,\"elapsedMs\":%u,\"buttons\":%u,\"forward\":%d,\"right\":%d,\"lookMilliDegrees\":[%d,%d],\"actions\":%u}\n",
        tape.Index(),tape.Clock(),elapsedMs,tapeInput.buttons,int(tapeInput.forward),int(tapeInput.right),
        int(tapeInput.pitchMilliDegrees),int(tapeInput.yawMilliDegrees),unsigned(tapeInput.actions));
    Write(line,size,sizeof(line));return true;
}
bool VitaCpuReplay_Input(VitaInputSample* sample) {
    if(!tapeActive || !sample)return false;
    *sample={};sample->buttons=tapeInput.buttons;
    sample->forwardmove=tapeInput.forward;sample->rightmove=tapeInput.right;
    // The first read consumes relative motion/edges. A second read of the
    // same command must not reapply them, just as in physical input handling.
    sample->lookDegrees[0]=float(tapeInput.pitchMilliDegrees)*0.001f;
    sample->lookDegrees[1]=float(tapeInput.yawMilliDegrees)*0.001f;
    sample->weaponCyclePressed=(tapeInput.actions&1)!=0;
    sample->rearFragPressed=(tapeInput.actions&2)!=0;
    sample->rearSpecialPressed=(tapeInput.actions&4)!=0;
    sample->rearFragHeld=(tapeInput.actions&8)!=0;
    sample->rearSpecialHeld=(tapeInput.actions&16)!=0;
    sample->rearMeleePressed=(tapeInput.actions&32)!=0;
    sample->rearSprintHeld=(tapeInput.actions&64)!=0;
    tapeInput.pitchMilliDegrees=tapeInput.yawMilliDegrees=0;tapeInput.actions&=~39u;
    tapeInput.buttons&=~BUTTON_MELEE;
    char line[128];const int size=std::snprintf(line,sizeof(line),
        "{\"type\":\"replay-input-read\",\"step\":%u,\"read\":%u}\n",tape.Index(),++inputReads);
    Write(line,size,sizeof(line));return true;
}
void VitaCpuReplay_EndTape(bool complete) {
    if(!tapeActive)return;
    VitaCpuReplay_Enable(false,complete && tape.Complete());tapeActive=false;
    // Stop the harness before source teardown. A later ordinary session must
    // inherit the real source clock rather than the tape's simulated time.
    cls.realtime=int(Sys_Milliseconds());com_frameTime=cls.realtime;
    cls.realFrametime=cls.frametime=0;
    VitaCpuProfile_Counter("source-replay-end",complete && tape.Complete(),tape.Index());
    VitaInput_ClearSourceSample();VitaCpuProfile_Flush(true);
}
void VitaCpuReplay_SourceInitialize() {
    ++sourceInitializations;
    if(tapeActive) {
        cls.realtime=int(tape.Clock());com_frameTime=int(tape.Clock());
    }
}
void VitaCpuReplay_State(const char* owner,const playerState_s& ps,uint32_t tick) {
    if(!enabled)return;
    // Individually selected fields exclude padding, pointers and unrelated UI.
    uint64_t movement=14695981039346656037ull,weapon=movement,events=movement;
#define FIELD(hash,member) hash=Hash(&ps.member,sizeof(ps.member),hash)
    FIELD(movement,commandTime);FIELD(movement,pm_type);FIELD(movement,pm_flags);FIELD(movement,pm_time);
    FIELD(movement,origin);FIELD(movement,velocity);FIELD(movement,groundEntityNum);FIELD(movement,viewangles);
    FIELD(movement,jumpTime);FIELD(movement,jumpOriginZ);FIELD(movement,stats);
    FIELD(weapon,weapFlags);FIELD(weapon,weapon);FIELD(weapon,weaponstate);FIELD(weapon,weaponTime);
    FIELD(weapon,weaponDelay);FIELD(weapon,weaponShotCount);FIELD(weapon,grenadeTimeLeft);
    FIELD(weapon,ammo);FIELD(weapon,ammoclip);FIELD(weapon,weapons);FIELD(weapon,weaponold);FIELD(weapon,weaponrechamber);
    FIELD(weapon,offHandIndex);FIELD(weapon,fWeaponPosFrac);FIELD(weapon,weapAnim);
    FIELD(events,eventSequence);FIELD(events,events);FIELD(events,eventParms);FIELD(events,oldEventSequence);
#undef FIELD
    char line[1024];const int size=std::snprintf(line,sizeof(line),
        "{\"type\":\"state\",\"owner\":\"%s\",\"tick\":%u,\"commandTime\":%d,\"movement\":\"%016llx\",\"weapon\":\"%016llx\",\"events\":\"%016llx\",\"crtRng\":%u,\"gameRng\":%u,\"eventOrder\":%u,\"origin\":[%.9g,%.9g,%.9g],\"velocity\":[%.9g,%.9g,%.9g],\"ground\":%d,\"weaponIndex\":%u,\"weaponState\":%d,\"weaponTime\":%d,\"health\":%d}\n",
        owner,tick,ps.commandTime,(unsigned long long)movement,(unsigned long long)weapon,(unsigned long long)events,
        sourceCrtRandom.State(),VitaCpuGameplayRandomState(),eventOrder,ps.origin[0],ps.origin[1],ps.origin[2],
        ps.velocity[0],ps.velocity[1],ps.velocity[2],ps.groundEntityNum,ps.weapon,int(ps.weaponstate),ps.weaponTime,ps.stats[0]);
    Write(line,size,sizeof(line));
}
void VitaCpuReplay_Command(const usercmd_s& cmd,uint32_t number) {
    if(!enabled)return;
    char line[384];const int size=std::snprintf(line,sizeof(line),
        "{\"type\":\"command\",\"number\":%u,\"time\":%d,\"buttons\":%u,\"weapon\":%u,\"offhand\":%u,\"forward\":%d,\"right\":%d,\"angles\":[%d,%d,%d]}\n",
        number,cmd.serverTime,cmd.buttons,cmd.weapon,cmd.offHandIndex,int(static_cast<signed char>(cmd.forwardmove)),
        int(static_cast<signed char>(cmd.rightmove)),cmd.angles[0],cmd.angles[1],cmd.angles[2]);
    Write(line,size,sizeof(line));
}
void VitaCpuReplay_ServerWorld(uint32_t tick) {
    if(!enabled)return;
    VITA_CPU_SCOPE("replay-world-observer");
    uint64_t hash=14695981039346656037ull;
    uint32_t count=0;
    for(int i=0;i<level.num_entities;++i) {
        const auto& entity=g_entities[i];
        if(!entity.r.inuse)continue;
        ++count;hash=Hash(&i,sizeof(i),hash);
        // Selected authored state only. No struct padding, allocator addresses,
        // inactive union storage, or new RNG calls enter this comparison.
#define ENTITY(member) hash=Hash(&entity.member,sizeof(entity.member),hash)
        ENTITY(s.number);ENTITY(s.eType);ENTITY(s.eventSequence);ENTITY(s.events);ENTITY(s.eventParms);
        ENTITY(r.linked);ENTITY(r.bmodel);ENTITY(r.svFlags);ENTITY(r.clientMask);
        ENTITY(r.contents);ENTITY(r.mins);ENTITY(r.maxs);ENTITY(r.absmin);ENTITY(r.absmax);
        ENTITY(r.currentOrigin);ENTITY(r.currentAngles);ENTITY(r.eventTime);
        ENTITY(model);ENTITY(physicsObject);ENTITY(takedamage);ENTITY(active);ENTITY(handler);
        ENTITY(team);ENTITY(spawnflags);ENTITY(flags);ENTITY(eventTime);ENTITY(freeAfterEvent);
        ENTITY(unlinkAfterEvent);ENTITY(clipmask);ENTITY(nextthink);ENTITY(health);ENTITY(maxHealth);
        ENTITY(damage);ENTITY(count);
#undef ENTITY
    }
    char line[256];const int size=std::snprintf(line,sizeof(line),
        "{\"type\":\"world\",\"tick\":%u,\"time\":%d,\"entities\":%u,\"state\":\"%016llx\",\"eventOrder\":%u}\n",
        tick,level.time,count,(unsigned long long)hash,eventOrder);
    Write(line,size,sizeof(line));
}
void VitaCpuReplay_Event(const char* owner,uint32_t entity,uint32_t event,uint32_t parameter) {
    if(!enabled)return;
    char line[256];const int size=std::snprintf(line,sizeof(line),
        "{\"type\":\"event\",\"order\":%u,\"owner\":\"%s\",\"entity\":%u,\"event\":%u,\"parameter\":%u}\n",
        ++eventOrder,owner,entity,event,parameter);
    Write(line,size,sizeof(line));
}
}
#endif
