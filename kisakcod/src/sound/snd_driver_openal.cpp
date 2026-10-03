#ifdef KISAK_OPENAL
#include <vita/vita_cpu_audio.h>
#include <vita/vita_cpu_profile.h>
#if KISAK_VITA_CPU_RUNTIME
#include <vita/vita_cpu_source_service.h>
static void SND_ServiceExistingStreams();
#endif

#include <universal/q_shared.h>
#include "snd_local.h"
#include "snd_public.h"
#include <qcommon/qcommon.h>
#include <universal/com_files.h>
#if !defined(KISAK_VITA_SLICE)
#include <gfx_d3d/r_cinematic.h>
#else
#include <win32/win_local.h>
#endif
#include <universal/com_sndalias.h>
#include <universal/profile.h>

#ifdef KISAK_MP
#include <cgame_mp/cg_local_mp.h>
#elif KISAK_SP
#include <cgame/cg_main.h>
#endif

#if KISAK_VITA_BENCHMARK
#include <vita/vita_audio_perf.h>
#include <vita/vita_benchmark_audio.h>
#include <vita/vita_script_services.h>
#include <psp2/kernel/processmgr.h>
static VitaAudioPerf audioPerf;
static uint32_t audioBenchmarkMode=0, audioBenchmarkErrors=0;
void VitaAudioPerf_Reset() { audioPerf={}; }
VitaAudioPerf VitaAudioPerf_Read();
#define AUDIO_TICK(name) const uint64_t name=sceKernelGetProcessTimeWide()
#define AUDIO_ADD(field,name) audioPerf.field += sceKernelGetProcessTimeWide()-name
#else
#define AUDIO_TICK(name)
#define AUDIO_ADD(field,name)
#endif
#include <dr_libs/dr_wav.h>
#include <dr_libs/dr_mp3.h>
#include <AL/efx-presets.h>
#include <fstream>

AlLocal alGlob;
#if KISAK_VITA_CPU_RUNTIME
void VitaCpuSound_EndContext();
namespace {
struct ResidentSound {const MssSoundCOD4* sound=nullptr;ALuint buffer=0;uint32_t refs=0,bytes=0;uint64_t used=0;};
ResidentSound soundCache[256]{};
ALuint streamPool[13][4]{};
uint64_t soundCacheBytes=0,soundCacheClock=0;
constexpr uint64_t kSoundCacheBytes=16*1024*1024;
ALuint SoundCacheFailure(const char* reason) {
    kisak::vita::VitaCpuProfile_Invalidate(reason);return 0;
}
ALuint AcquireSound(const MssSoundCOD4* sound,ALenum format) {
    VITA_CPU_SCOPE("loaded-sound-buffer");
    for(auto& entry:soundCache)if(entry.sound==sound && entry.buffer) {
        ++entry.refs;entry.used=++soundCacheClock;
        kisak::vita::VitaCpuProfile_Counter("loaded-sound-reuse",soundCacheBytes,entry.refs);return entry.buffer;
    }
    const uint32_t bytes=sound->info.data_len;
    if(bytes>kSoundCacheBytes){kisak::vita::VitaCpuProfile_Counter("loaded-sound-budget-failed",bytes);return SoundCacheFailure("loaded-sound-budget-exhausted");}
    ResidentSound* destination=nullptr;
    for(;;) {
        destination=nullptr;
        for(auto& entry:soundCache)if(!entry.refs && (!destination || entry.used<destination->used))destination=&entry;
        if(!destination)return SoundCacheFailure("loaded-sound-slots-exhausted");
        if(destination->buffer){alDeleteBuffers(1,&destination->buffer);soundCacheBytes-=destination->bytes;*destination={};}
        if(soundCacheBytes+bytes<=kSoundCacheBytes)break;
        // Evict another unreferenced resident; empty entries cannot free bytes.
        ResidentSound* oldest=nullptr;
        for(auto& entry:soundCache)if(entry.buffer && !entry.refs && (!oldest || entry.used<oldest->used))oldest=&entry;
        if(!oldest)return SoundCacheFailure("loaded-sound-residency-exhausted");
        alDeleteBuffers(1,&oldest->buffer);soundCacheBytes-=oldest->bytes;*oldest={};
    }
    ALuint buffer=0;alGenBuffers(1,&buffer);
    if(!buffer)return SoundCacheFailure("loaded-sound-allocation-failed");
    alBufferData(buffer,format,sound->data,bytes,sound->info.rate);
    if(alGetError()!=AL_NO_ERROR){alDeleteBuffers(1,&buffer);return SoundCacheFailure("loaded-sound-upload-failed");}
    *destination={sound,buffer,1,bytes,++soundCacheClock};soundCacheBytes+=bytes;
    kisak::vita::VitaCpuProfile_Counter("loaded-sound-resident",soundCacheBytes,bytes);return buffer;
}
}
void VitaCpuSound_EndContext() {
    kisak::vita::VitaCpuAudio_Drain();
    for(auto& entry:soundCache){if(entry.buffer)alDeleteBuffers(1,&entry.buffer);entry={};}
    for(auto& pool:streamPool){alDeleteBuffers(4,pool);std::fill_n(pool,4,0u);}
    soundCacheBytes=soundCacheClock=0;
}
#endif

const dvar_t *snd_khz;
const dvar_t *snd_outputConfiguration;

void __cdecl TRACK_snd_driver()
{
    track_static_alloc_internal(&alGlob, sizeof(alGlob), "alGlob", 13);
}

bool __cdecl SND_IsMultiChannel()
{
    return alGlob.isMultiChannel;
}

char __cdecl SND_InitDriver()
{
#if defined(KISAK_VITA_SLICE)
    snd_khz = Dvar_RegisterInt("snd_khz", 48, 48, 48, DVAR_ROM, "Native Vita output frequency (source samples retain their rates).");
#else
    snd_khz = Dvar_RegisterInt("snd_khz", 44, (DvarLimits)0x2C0000000BLL, DVAR_ARCHIVE | DVAR_LATCH, "The game sound frequency.");
#endif
    snd_outputConfiguration = Dvar_RegisterEnum(
        "snd_outputConfiguration",
        snd_outputConfigurationStrings,
        0,
        DVAR_ARCHIVE | DVAR_LATCH,
        "Sound output configuration");

    if (MSS_Startup())
    {
        if (MSS_Init())
        {
            MSS_InitChannels();
#if KISAK_VITA_CPU_RUNTIME
            if(!kisak::vita::VitaCpuAudio_Start()){MSS_ShutdownCleanup();MSS_InitFailed();return 0;}
            for(auto& pool:streamPool)alGenBuffers(4,pool);
            bool poolReady=alGetError()==AL_NO_ERROR;
            for(const auto& pool:streamPool)for(auto buffer:pool)poolReady=poolReady && buffer!=0;
            if(!poolReady) {
                kisak::vita::VitaCpuProfile_Invalidate("stream-buffer-pool-allocation-failed");
                MSS_ShutdownCleanup();MSS_InitFailed();return 0;
            }
#endif
            MSS_InitEq();
#if KISAK_VITA_CPU_RUNTIME
            kisak::vita::VitaCpuSource_SetService(SND_ServiceExistingStreams);
#endif
            return 1;
        }
        else
        {
            MSS_ShutdownCleanup();
            MSS_InitFailed();
            return 0;
        }
    }
    else
    {
        MSS_InitFailed();
        return 0;
    }
}

void __cdecl SND_ShutdownDriver()
{
#if !defined(KISAK_VITA_SLICE)
    R_Cinematic_StopPlayback();
    R_Cinematic_SyncNow();
#endif
    MSS_ShutdownCleanup();
}

int __cdecl SND_GetDriverCPUPercentage()
{
    // KISAK_OPENAL TODO: no direct OpenAL equivalent to Miles' per-driver CPU usage stat.
    return 0;
}

void __cdecl SND_Set3DPosition(int index, const float *org)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    float delta[3];
    int listenerIndex = SND_GetListenerIndexNearestToOrigin(org);
    Vec3Sub(org, g_snd.listeners[listenerIndex].orient.origin, delta);
    float transformed[3];
    MatrixTransposeTransformVector(delta, g_snd.listeners[listenerIndex].orient.axis, transformed);
    alSource3f(alGlob.source[index], AL_POSITION, -transformed[1], transformed[2], -transformed[0]);
}

#ifdef KISAK_OPENAL
void SND_ReleaseChannelBuffer(int index)
{
    if (alGlob.channelBuffer[index])
    {
#if KISAK_VITA_CPU_RUNTIME
        alSourceStop(alGlob.source[index]);
        alSourcei(alGlob.source[index],AL_BUFFER,0);
        for(auto& entry:soundCache)if(entry.buffer==alGlob.channelBuffer[index]) {
            if(entry.refs)--entry.refs;
            break;
        }
#else
        alSourcei(alGlob.source[index], AL_BUFFER, 0);
        alDeleteBuffers(1, &alGlob.channelBuffer[index]);
#endif
        alGlob.channelBuffer[index] = 0;
    }
}

// Defined near SND_SetRoomtype (Phase 6) below; forward-declared here since
// SND_StartAlias2D/3DSample and SND_StartAliasStreamOnChannel call it before that point.
void SND_ApplyReverbSend(int index, const snd_alias_t *alias);

struct AlStreamState
{
    bool active;
#if KISAK_VITA_BENCHMARK
    bool benchmarkEligible, benchmarkPcm, benchmarkSuppressed;
    uint32_t benchmarkNameHash;
#endif
    bool isMp3;
    bool looping;
#if KISAK_VITA_CPU_RUNTIME
    uint32_t generation;
    uint64_t totalFrames;
#else
    int fsHandle;
    drwav wav;
    drmp3 mp3;
#endif
    uint32_t channels;
    uint32_t sampleRate;
    // Frames decoded/queued since the current loop pass began (reset to the post-seek frame
    // index on start, reset again each time SND_FillStreamBuffers loops back to frame 0).
    // Needed because AL_SEC_OFFSET on a queued streaming source only reports the offset
    // *within the currently-playing buffer chunk*, not a running position across the whole
    // stream - there's no direct AL query for that, so SND_GetStreamChannelSaveInfo's
    // fraction is approximated from this instead (see its comment for the small, bounded
    // error this introduces: up to one buffer queue's worth of look-ahead).
    uint64_t framesQueued;
    // Recycle the four queue buffers for this stream's lifetime. Recreating
    // them on each refill churns allocator/backend state on the main thread.
    ALuint buffers[4];
    ALuint available[4];
    unsigned bufferCount;
    unsigned availableCount;
#if KISAK_VITA_CPU_RUNTIME
    bool observedPlaying;
    bool underrunActive;
    uint64_t lastServiceUs;
#endif
};

// Only the 13 source stream channels need decoders. Keep their public channel
// indices (40-52) unchanged without reserving 40 unused MP3 decoder objects.
// PCM voices continue to use the original source/channel arrays.
static AlStreamState g_streamState[13];
#if KISAK_VITA_BENCHMARK
VitaAudioPerf VitaAudioPerf_Read() {
    auto result=audioPerf;
    result.mode=audioBenchmarkMode; result.errors=audioBenchmarkErrors;
    for (unsigned i=0;i<13;++i) {
        const auto& stream=g_streamState[i];
        auto& out=result.streams[i];
        out.active=stream.active;
        if (!stream.active) continue;
        ++result.activeStreams;
        out.eligible=stream.benchmarkEligible; out.pcm=stream.benchmarkPcm;
        out.suppressed=stream.benchmarkSuppressed; out.nameHash=stream.benchmarkNameHash;
        out.rate=stream.sampleRate; out.channels=stream.channels;
        result.eligibleStreams+=out.eligible; result.pcmStreams+=out.pcm;
        result.suppressedStreams+=out.suppressed;
    }
    return result;
}
#endif

static AlStreamState *SND_StreamState(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + 13);
    return &g_streamState[index - SND_FIRST_STREAM_CHANNEL];
}

static const int AL_STREAM_BUFFER_COUNT = 4;
static const int AL_STREAM_BUFFER_FRAMES = 8192;

// Miles' MSS_File*Callback (snd_mss.cpp) bridges AIL's file I/O to FS_*; these do the same
// for dr_wav/dr_mp3, since neither library has a concept of a game virtual filesystem.
// Format extension is reachable from data (see WORK.md Phase 5 - Com_GetSoundFileName's
// filename comes straight from the asset CSV, not hardcoded to .wav), so both are wired up.
#if !KISAK_VITA_CPU_RUNTIME
static size_t AL_StreamReadCallback(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    AlStreamState *stream = (AlStreamState *)pUserData;
    AUDIO_TICK(readStart);
    const auto read=FS_Read((uint8_t *)pBufferOut, (uint32_t)bytesToRead, stream->fsHandle);
    AUDIO_ADD(readUs,readStart);
#if KISAK_VITA_BENCHMARK
    if (read > 0) audioPerf.readBytes += read;
    auto& detail=audioPerf.streams[stream-g_streamState];
    detail.readUs+=sceKernelGetProcessTimeWide()-readStart;
    if (read>0) detail.readBytes+=read;
    ++detail.readCalls;
#endif
    return read;
}

// FS_Seek's zip-archive branch (com_files.cpp, used whenever a streamed sound is packed into an
// .iwd rather than sitting as a loose file on disk) doesn't implement standard SEEK_SET/CUR/END
// semantics - its three origin values are rotated by one relative to what every caller (including
// the original Miles snd_mss.cpp callback, and dr_wav/dr_mp3 here) assumes:
//   want SEEK_SET (absolute)         -> must pass origin 2 to FS_Seek
//   want SEEK_CUR (relative-to-here) -> must pass origin 0 to FS_Seek
//   want SEEK_END (relative-to-EOF)  -> must pass origin 1 to FS_Seek
// This is inherited unmodified from the original decompiled engine, so it isn't safe to fix inside
// FS_Seek itself without risking every other zip-file seek in the game (save data, Miles' own
// zip-streamed audio, etc.) - instead, rotate only here, and only for zip-backed handles; loose
// files go through a plain FS_FileSeek/fseek and are unaffected by this.
static int AL_FixupZipSeekOrigin(int fsHandle, int trueOrigin)
{
    static const int kZipOriginRotation[3] = { 2, 0, 1 }; // SEEK_SET->2, SEEK_CUR->0, SEEK_END->1
    return FS_IsFileInZip(fsHandle) ? kZipOriginRotation[trueOrigin] : trueOrigin;
}

static drwav_bool32 AL_WavSeekCallback(void *pUserData, int offset, drwav_seek_origin origin)
{
    AlStreamState *stream = (AlStreamState *)pUserData;
    FS_Seek(stream->fsHandle, offset, AL_FixupZipSeekOrigin(stream->fsHandle, (int)origin));
    return DRWAV_TRUE;
}

static drwav_bool32 AL_WavTellCallback(void *pUserData, drwav_int64 *pCursor)
{
    AlStreamState *stream = (AlStreamState *)pUserData;
    *pCursor = FS_FTell(stream->fsHandle);
    return DRWAV_TRUE;
}

static drmp3_bool32 AL_Mp3SeekCallback(void *pUserData, int offset, drmp3_seek_origin origin)
{
    AlStreamState *stream = (AlStreamState *)pUserData;
    FS_Seek(stream->fsHandle, offset, AL_FixupZipSeekOrigin(stream->fsHandle, (int)origin));
    return DRMP3_TRUE;
}

static drmp3_bool32 AL_Mp3TellCallback(void *pUserData, drmp3_int64 *pCursor)
{
    AlStreamState *stream = (AlStreamState *)pUserData;
    *pCursor = FS_FTell(stream->fsHandle);
    return DRMP3_TRUE;
}

#endif
// Stops, unqueues/deletes all buffers, closes the decoder and file handle for a stream
// channel. Safe to call on an already-inactive channel (no-op).
static void SND_ReleaseStreamChannel(int index)
{
    AlStreamState *stream = SND_StreamState(index);
    if (!stream->active)
        return;

    alSourceStop(alGlob.source[index]);
    ALint queued = 0;
    alGetSourcei(alGlob.source[index], AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0)
    {
        ALuint buf;
        alSourceUnqueueBuffers(alGlob.source[index], 1, &buf);

    }
    alSourcei(alGlob.source[index], AL_BUFFER, 0);
#if !KISAK_VITA_CPU_RUNTIME
    if (stream->bufferCount) alDeleteBuffers(stream->bufferCount, stream->buffers);
#endif
    stream->bufferCount = stream->availableCount = 0;

#if KISAK_VITA_CPU_RUNTIME
    kisak::vita::VitaCpuAudio_Close(index-SND_FIRST_STREAM_CHANNEL,stream->generation);
#else
    if (stream->isMp3)
        drmp3_uninit(&stream->mp3);
    else
        drwav_uninit(&stream->wav);
    FS_FCloseFile(stream->fsHandle);
#endif
    stream->active = false;
}

// Decodes and queues fresh PCM chunks to bring the channel's buffer queue back up to
// AL_STREAM_BUFFER_COUNT, first reclaiming any buffers that finished playing.
// Handles looping by seeking back to the start of the audio data when the decoder runs dry.
// Returns false once the stream has ended (not looping) with nothing left queued - the
// caller uses that to know when the channel should stop/free itself.
static bool SND_FillStreamBuffers(int index)
{
    AlStreamState *stream = SND_StreamState(index);
    if (!stream->active)
        return false;

#if KISAK_VITA_BENCHMARK
    auto& detail=audioPerf.streams[index-SND_FIRST_STREAM_CHANNEL];
    ++detail.refillCalls;
    if (stream->benchmarkSuppressed) return true;
#endif
    ALuint source = alGlob.source[index];

    AUDIO_TICK(queryStart);
    ALint processed = 0;
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
#if KISAK_VITA_BENCHMARK
    detail.processedBuffers+=static_cast<uint32_t>(std::max(processed,0));
#endif
    while (processed-- > 0)
    {
        ALuint buf;
        alSourceUnqueueBuffers(source, 1, &buf);
        iassert(stream->availableCount < AL_STREAM_BUFFER_COUNT);
        stream->available[stream->availableCount++] = buf;
    }

    ALint queued = 0;
    alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
#if KISAK_VITA_BENCHMARK
    ALint sourceState=0;
    alGetSourcei(source,AL_SOURCE_STATE,&sourceState);
    if (stream->looping && queued>0 && sourceState==AL_STOPPED
        && !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay) ++detail.stoppedQueued;
#endif
    AUDIO_ADD(queueUs,queryStart);

    // Reused across calls rather than stack-allocated (32KB) each time; safe because stream
    // channels are always filled one at a time, never interleaved, on a single thread.
    static int16_t chunk[AL_STREAM_BUFFER_FRAMES * 2]; // *2 for stereo worst case

    for (int i = queued; i < AL_STREAM_BUFFER_COUNT; ++i)
    {
        AUDIO_TICK(decodeStart);
#if KISAK_VITA_CPU_RUNTIME
        uint32_t decodedFrames=0;bool looped=false;
        const auto pcm=kisak::vita::VitaCpuAudio_Read(index-SND_FIRST_STREAM_CHANNEL,
            stream->generation,chunk,&decodedFrames,&looped);
        if(pcm==kisak::vita::VitaCpuPcmResult::Pending)return true;
        if(pcm==kisak::vita::VitaCpuPcmResult::Error || pcm==kisak::vita::VitaCpuPcmResult::End)break;
        const uint64_t framesRead=decodedFrames;
        if(looped)stream->framesQueued=0;
#else
        uint64_t framesRead = stream->isMp3
            ? drmp3_read_pcm_frames_s16(&stream->mp3, AL_STREAM_BUFFER_FRAMES, chunk)
            : drwav_read_pcm_frames_s16(&stream->wav, AL_STREAM_BUFFER_FRAMES, chunk);

        if (framesRead == 0)
        {
            if (!stream->looping)
                break;
            if (stream->isMp3)
                drmp3_seek_to_pcm_frame(&stream->mp3, 0);
            else
                drwav_seek_to_pcm_frame(&stream->wav, 0);
            framesRead = stream->isMp3
                ? drmp3_read_pcm_frames_s16(&stream->mp3, AL_STREAM_BUFFER_FRAMES, chunk)
                : drwav_read_pcm_frames_s16(&stream->wav, AL_STREAM_BUFFER_FRAMES, chunk);
            if (framesRead == 0)
                break; // zero-length file - avoid spinning forever
            stream->framesQueued = 0; // starting a fresh loop pass
        }

#endif
        AUDIO_ADD(decodeUs,decodeStart);
#if KISAK_VITA_BENCHMARK
        audioPerf.frames += framesRead;
        detail.frames+=framesRead; detail.decodeUs+=sceKernelGetProcessTimeWide()-decodeStart;
#endif
        ALuint buffer = 0;
        if (stream->availableCount) {
            buffer = stream->available[--stream->availableCount];
        } else if (stream->bufferCount < AL_STREAM_BUFFER_COUNT) {
            alGenBuffers(1, &buffer);
            if (!buffer) break;
            stream->buffers[stream->bufferCount++] = buffer;
        } else {
            break;
        }
        ALenum format = (stream->channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
        AUDIO_TICK(uploadStart);
        alBufferData(buffer, format, chunk, (ALsizei)(framesRead * stream->channels * sizeof(int16_t)), stream->sampleRate);
        AUDIO_ADD(uploadUs,uploadStart);
#if KISAK_VITA_BENCHMARK
        ++detail.uploads;
#endif
        AUDIO_TICK(queueStart);
        alSourceQueueBuffers(source, 1, &buffer);
        AUDIO_ADD(queueUs,queueStart);
        stream->framesQueued += framesRead;
        ++queued;
    }

    return queued > 0;
}
#endif

#if KISAK_VITA_CPU_RUNTIME
static void SND_ResumeExistingStream(int i,bool hasMoreToPlay,ALint state) {
    auto* cpuStream=SND_StreamState(i);
    const auto now=kisak::vita::VitaCpuNow();
        if(state==AL_PLAYING){cpuStream->observedPlaying=true;cpuStream->underrunActive=false;}
        if(state==AL_STOPPED && !g_snd.chaninfo[i].startDelay && hasMoreToPlay) {
            if(cpuStream->observedPlaying && !cpuStream->underrunActive) {
                kisak::vita::VitaCpuAudio_ReportUnderrun(i-SND_FIRST_STREAM_CHANNEL);
                kisak::vita::VitaCpuProfile_Counter("audio-underrun-service-gap-us",
                    cpuStream->lastServiceUs?now-cpuStream->lastServiceUs:0,i-SND_FIRST_STREAM_CHANNEL);
                cpuStream->underrunActive=true;
            }
            ALint queued=0;alGetSourcei(alGlob.source[i],AL_BUFFERS_QUEUED,&queued);
            if(queued>0)alSourcePlay(alGlob.source[i]);
        }
    cpuStream->lastServiceUs=now;
}
static void SND_ServiceExistingStreams() {
    VITA_CPU_SCOPE("source-loading-audio-service");
    for(int i=SND_FIRST_STREAM_CHANNEL;i<SND_FIRST_STREAM_CHANNEL+g_snd.max_stream_channels;++i) {
        // Refill/restart only existing, undelayed, unpaused streams. End/chain,
        // spatial controls and sound-event advancement stay in SND_Update.
        if(!SND_StreamState(i)->active || g_snd.chaninfo[i].paused || g_snd.chaninfo[i].startDelay)continue;
        const bool more=SND_FillStreamBuffers(i);
        ALint state;alGetSourcei(alGlob.source[i],AL_SOURCE_STATE,&state);
        SND_ResumeExistingStream(i,more,state);
    }
}
#endif

void __cdecl SND_Stop2DChannel(int index)
{
    iassert((index >= 0 && index < 0 + g_snd.max_2D_channels));
    SND_ReleaseChannelBuffer(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

void __cdecl SND_Pause2DChannel(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);
    alSourcePause(alGlob.source[index]);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_Unpause2DChannel(int index, int timeshift)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    if (!g_snd.chaninfo[index].startDelay)
    {
        alSourcePlay(alGlob.source[index]);
    }

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_Is2DChannelFree(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_Stop3DChannel(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    SND_ReleaseChannelBuffer(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

void __cdecl SND_Pause3DChannel(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);
    alSourcePause(alGlob.source[index]);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_Unpause3DChannel(int index, int timeshift)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    if (!g_snd.chaninfo[index].startDelay)
    {
        alSourcePlay(alGlob.source[index]);
    }

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_Is3DChannelFree(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    return !g_snd.chaninfo[index].paused && !g_snd.chaninfo[index].startDelay && g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_StopStreamChannel(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    SND_ReleaseStreamChannel(index);
    SND_ResetChannelInfo(index);
    SND_RemoveVoice(g_snd.chaninfo[index].entchannel);
}

#if KISAK_VITA_BENCHMARK
void VitaAudioBenchmark_SetMode(uint32_t mode) {
    if (mode==audioBenchmarkMode) return;
    for (int i=0;i<13;++i)
        if (g_streamState[i].active && g_streamState[i].benchmarkEligible)
            SND_StopStreamChannel(SND_FIRST_STREAM_CHANNEL+i);
    audioBenchmarkMode=mode; audioBenchmarkErrors=mode>2?1:0;
}
#endif

void __cdecl SND_PauseStreamChannel(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);
    alSourcePause(alGlob.source[index]);
    g_snd.chaninfo[index].paused = 1;
}

void __cdecl SND_UnpauseStreamChannel(int index, int timeshift)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    if (!g_snd.chaninfo[index].startDelay)
    {
        alSourcePlay(alGlob.source[index]);
    }

    g_snd.chaninfo[index].soundFileInfo.endtime += timeshift;
    g_snd.chaninfo[index].startTime += timeshift;
    g_snd.chaninfo[index].paused = 0;
}

bool __cdecl SND_IsStreamChannelFree(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    if (!SND_StreamState(index)->active)
        return 1;

    if (g_snd.chaninfo[index].paused || g_snd.chaninfo[index].startDelay)
        return 0;

    return g_snd.chaninfo[index].alias0 == 0;
}

void __cdecl SND_ApplyChannelMap(ALuint handle, const snd_alias_t *alias, int srcChannelCount)
{
    // KISAK_OPENAL TODO (Phase 4): per-speaker channel-level mapping. 
}

int __cdecl SND_StartAlias2DSample(SndStartAliasInfo *startAliasInfo, int *pChannel)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile);
    iassert(startAliasInfo->alias0->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias0->soundFile->exists);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile);
    iassert(startAliasInfo->alias1->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias1->soundFile->exists);

    int entchannel = SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags);
    if (!SND_HasFreeVoice(entchannel))
        return -1;

    int index = SND_FindFree2DChannel(startAliasInfo, entchannel);
    if (pChannel)
        *pChannel = index;

    if (index < 0)
        return -1;

    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    ALuint source = alGlob.source[index];
    MssSoundCOD4 *sound = &startAliasInfo->alias0->soundFile->u.loadSnd->sound;

    // dr_wav (Phase 3) always decoded loaded sounds to 16-bit PCM, so the format is always
    // mono/stereo 16-bit - no need to replicate MSS_DigitalFormatType's branching here.
    SND_ReleaseChannelBuffer(index);
    ALenum format = (sound->info.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
#if KISAK_VITA_CPU_RUNTIME
    ALuint buffer=AcquireSound(sound,format);
    if(!buffer){kisak::vita::VitaCpuProfile_Counter("loaded-sound-allocation-failed",index);return SND_SetPlaybackIdNotPlayed(index);}
#else
    ALuint buffer;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, sound->data, sound->info.data_len, sound->info.rate);
#endif
    alGlob.channelBuffer[index] = buffer;
    alSourcei(source, AL_BUFFER, buffer);

    MSS_ApplyEqFilter(source, entchannel);

    // AL_PITCH is already a ratio relative to the buffer's native (embedded) rate, unlike
    // Miles' AIL_set_sample_playback_rate which needed the current absolute rate multiplied
    // out by hand - so this is simpler than the Miles branch above, not just a translation.
    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    alSourcef(source, AL_PITCH, pitch);

    float realVolume = startAliasInfo->volume
        * g_snd.volume
        * g_snd.channelvol->channelvol[SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags)].volume;

    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    SND_ApplyChannelMap(source, startAliasInfo->alias0, sound->info.channels);
    SND_Set2DChannelVolume(index, realVolume);
    alSourcei(source, AL_LOOPING, (startAliasInfo->alias0->flags & 1) != 0 ? AL_TRUE : AL_FALSE); // matches AIL_set_sample_loop_count's polarity above: flags bit 0 set == looping
    SND_ApplyReverbSend(index, startAliasInfo->alias0);

    // Duration at the pitch-adjusted rate, not the buffer's native rate - matches Miles'
    // behavior above, which queries AIL_sample_ms_position() *after* setting the pitched
    // playback rate, so a pitched-up sound reports a proportionally shorter duration.
    float effectiveRate = sound->info.rate * pitch;
    int total_msec = effectiveRate > 0.0f ? (int)((int64_t)sound->info.samples * 1000 / effectiveRate) : 0;

    if (startAliasInfo->timeshift >= total_msec)
        return SND_SetPlaybackIdNotPlayed(index);

    int start_msec;
    if (startAliasInfo->fraction == 0.0)
    {
        if (startAliasInfo->timeshift)
        {
            start_msec = startAliasInfo->timeshift;
        }
        else if ((startAliasInfo->alias0->flags & 0x20) != 0)
        {
            start_msec = SnapFloatToInt(random() * (float)total_msec) & 0xFFFFFF80;
        }
        else
        {
            start_msec = 0;
        }
    }
    else
    {
        start_msec = SnapFloatToInt((float)total_msec * startAliasInfo->fraction);
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

    alSourcef(source, AL_SEC_OFFSET, start_msec / 1000.0f);
    if (!startAliasInfo->startDelay
        && (!g_snd.paused || !g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8]))
    {
        alSourcePlay(source);
    }

    total_msec += startAliasInfo->startDelay;
    if ((startAliasInfo->alias0->flags & 1) != 0)
        total_msec = 0;
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, sound->info.channels, sound->info.rate, total_msec, start_msec, SFLS_LOADED);
    int playbackId = SND_AcquirePlaybackId(index, total_msec);

    if (playbackId != -1)
        SND_AddVoice(entchannel);

    return playbackId;
}

void __cdecl SND_Apply3DSpatializationTweaks(ALuint handle, const snd_alias_t *alias)
{
    // KISAK_OPENAL TODO (Phase 4)
}

int __cdecl SND_StartAlias3DSample(SndStartAliasInfo *startAliasInfo, int *pChannel)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile);
    iassert(startAliasInfo->alias0->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias0->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias0->soundFile->exists);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile);
    iassert(startAliasInfo->alias1->soundFile->type == SAT_LOADED);
    iassert(startAliasInfo->alias1->soundFile->u.loadSnd);
    iassert(startAliasInfo->alias1->soundFile->exists);

    int entchannel = (startAliasInfo->alias0->flags & 0x3F00) >> 8;
    if (!SND_HasFreeVoice(entchannel))
        return -1;
    int index = SND_FindFree3DChannel(startAliasInfo, entchannel);
    if (pChannel)
        *pChannel = index;
    if (index < 0)
        return -1;
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALuint source = alGlob.source[index];
    MssSoundCOD4 *sound = &startAliasInfo->alias0->soundFile->u.loadSnd->sound;
    float distMin = (1.0f - startAliasInfo->lerp) * startAliasInfo->alias0->distMin
        + startAliasInfo->alias1->distMin * startAliasInfo->lerp;
    float distMax = (1.0f - startAliasInfo->lerp) * startAliasInfo->alias0->distMax
        + startAliasInfo->alias1->distMax * startAliasInfo->lerp;

    SND_ReleaseChannelBuffer(index);
    ALenum format = (sound->info.channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
#if KISAK_VITA_CPU_RUNTIME
    ALuint buffer=AcquireSound(sound,format);
    if(!buffer){kisak::vita::VitaCpuProfile_Counter("loaded-sound-allocation-failed",index);return SND_SetPlaybackIdNotPlayed(index);}
#else
    ALuint buffer;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, sound->data, sound->info.data_len, sound->info.rate);
#endif
    alGlob.channelBuffer[index] = buffer;
    alSourcei(source, AL_BUFFER, buffer);

    MSS_ApplyEqFilter(source, entchannel);

    const float *listener = g_snd.listeners[SND_GetListenerIndexNearestToOrigin(startAliasInfo->org)].orient.origin;
    float diff[3];
    Vec3Sub(listener, startAliasInfo->org, diff);
    float distance = Vec3Length(diff);
    float attenuation = SND_Attenuate(startAliasInfo->alias0->volumeFalloffCurve, distance, distMin, distMax);
    float realVolume = startAliasInfo->volume
        * attenuation
        * g_snd.channelvol->channelvol[(startAliasInfo->alias0->flags & 0x3F00) >> 8].volume;
    realVolume = realVolume * g_snd.volume;
    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    SND_Apply3DSpatializationTweaks(source, startAliasInfo->alias0);
    SND_Set3DChannelVolume(index, realVolume);
    // distMin/distMax feed OpenAL's automatic distance-attenuation model, which is disabled
    // (alDistanceModel(AL_NONE), see MSS_Init) since SND_Attenuate's curve above already
    // computes the final gain by hand - so unlike Miles' AIL_set_sample_3D_distances there's
    // nothing to set here at all.

    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    alSourcef(source, AL_PITCH, pitch);
    SND_Set3DPosition(index, startAliasInfo->org);
    alSourcei(source, AL_LOOPING, (startAliasInfo->alias0->flags & 1) != 0 ? AL_TRUE : AL_FALSE); // matches AIL_set_sample_loop_count's polarity above: flags bit 0 set == looping
    SND_ApplyReverbSend(index, startAliasInfo->alias0);

    // Duration at the pitch-adjusted rate - see the matching comment in
    // SND_StartAlias2DSample above.
    float effectiveRate = sound->info.rate * pitch;
    int total_msec = effectiveRate > 0.0f ? (int)((int64_t)sound->info.samples * 1000 / effectiveRate) : 0;

    if (startAliasInfo->timeshift >= total_msec)
        return SND_SetPlaybackIdNotPlayed(index);

    int start_msec;
    if (startAliasInfo->fraction == 0.0)
    {
        if (startAliasInfo->timeshift)
        {
            start_msec = startAliasInfo->timeshift;
        }
        else if ((startAliasInfo->alias0->flags & 0x20) != 0)
        {
            start_msec = SnapFloatToInt(random() * (float)total_msec) & 0xFFFFFF80;
        }
        else
        {
            start_msec = 0;
        }
    }
    else
    {
        start_msec = SnapFloatToInt((float)total_msec * startAliasInfo->fraction);
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

    alSourcef(source, AL_SEC_OFFSET, start_msec / 1000.0f);
    if (!startAliasInfo->startDelay
        && (!g_snd.paused || !g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8]))
    {
        alSourcePlay(source);
    }
    int totalMsecForChan = total_msec + startAliasInfo->startDelay;
    if ((startAliasInfo->alias0->flags & 1) != 0)
        totalMsecForChan = 0;
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, sound->info.channels, sound->info.rate, totalMsecForChan, start_msec, SFLS_LOADED);
    int playbackId = SND_AcquirePlaybackId(index, totalMsecForChan);
    if (playbackId != -1)
        SND_AddVoice(entchannel);
    return playbackId;
}

void __cdecl SND_Set3DStreamPosition(int index, int listenerIndex, const float *org)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    float delta[3];
    Vec3Sub(org, g_snd.listeners[listenerIndex].orient.origin, delta);
    float transformed[3];
    MatrixTransposeTransformVector(delta, g_snd.listeners[listenerIndex].orient.axis, transformed);
    alSource3f(alGlob.source[index], AL_POSITION, -transformed[1], transformed[2], -transformed[0]);
}

double __cdecl SND_GetStream3DVolumeFallOff(int index, int listenerIndex)
{
    float diff[3]; // [esp+10h] [ebp-24h] BYREF
    float maxdist; // [esp+1Ch] [ebp-18h]
    float dist; // [esp+20h] [ebp-14h]
    float lerp; // [esp+24h] [ebp-10h]
    const snd_alias_t *alias1; // [esp+28h] [ebp-Ch]
    const snd_alias_t *alias0; // [esp+2Ch] [ebp-8h]
    float mindist; // [esp+30h] [ebp-4h]

    iassert(index >= ((0 + 8) + 32) && index < g_snd.max_stream_channels + ((0 + 8) + 32));

    alias0 = g_snd.chaninfo[index].alias0;
    alias1 = g_snd.chaninfo[index].alias1;
    if (!SND_IsAliasChannel3D(SNDALIASFLAGS_GET_CHANNEL(alias0->flags)))
        MyAssertHandler(
            ".\\win32\\snd_driver.cpp",
            585,
            0,
            "%s",
            "SND_IsAliasChannel3D( SNDALIASFLAGS_GET_CHANNEL( alias0->flags ) )");
    Vec3Sub(g_snd.listeners[listenerIndex].orient.origin, g_snd.chaninfo[index].org, diff);
    dist = Vec3Length(diff);
    lerp = g_snd.chaninfo[index].lerp;
    mindist = (1.0 - lerp) * alias0->distMin + alias1->distMin * lerp;
    maxdist = (1.0 - lerp) * alias0->distMax + alias1->distMax * lerp;
    return SND_Attenuate(alias0->volumeFalloffCurve, dist, mindist, maxdist);
}


int __cdecl SND_StartAliasStreamOnChannel(SndStartAliasInfo *startAliasInfo, int index)
{
    iassert(startAliasInfo->alias0);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias0->flags) == SAT_STREAMED);
    iassert(startAliasInfo->alias1);
    iassert(SNDALIASFLAGS_GET_TYPE(startAliasInfo->alias1->flags) == SAT_STREAMED);
    iassert((index >= ((0 + 8) + 32) && index < ((0 + 8) + 32) + g_snd.max_stream_channels));

    bool fsInitialized = FS_Initialized();
    iassert(fsInitialized);

    int entchannel = SNDALIASFLAGS_GET_CHANNEL(startAliasInfo->alias0->flags);
    if (!SND_HasFreeVoice(entchannel))
        return -1;

    char filename[128];
    Com_GetSoundFileName(startAliasInfo->alias0, filename, 128);

    if (!startAliasInfo->alias0->soundFile->exists)
    {
        Com_DPrintf(
            9,
            "Tried to play streamed sound '%s' from alias '%s', but it was not found at load time.\n",
            filename,
            startAliasInfo->alias0->aliasName);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    char realname[256];
    Com_sprintf(realname, 0x100u, "sound/%s", filename);

    SND_ReleaseStreamChannel(index); // close any previous stream still open on this channel

    AlStreamState *stream = SND_StreamState(index);
    memset(stream, 0, sizeof(*stream));

#if KISAK_VITA_BENCHMARK
    stream->benchmarkEligible=VitaBenchmarkBackgroundStream(filename,(startAliasInfo->alias0->flags&1)!=0);
    stream->benchmarkPcm=audioBenchmarkMode==2 && stream->benchmarkEligible && VitaBenchmarkPcmStream(filename);
    stream->benchmarkSuppressed=audioBenchmarkMode==1 && stream->benchmarkEligible;
    stream->benchmarkNameHash=VitaBenchmarkAudioNameHash(filename);
    if (stream->benchmarkPcm) Com_sprintf(realname,sizeof(realname),"sound/%s.bench.wav",filename);
#endif
#if KISAK_VITA_CPU_RUNTIME
    const size_t nameLen=strlen(realname);
    stream->isMp3=nameLen>=4 && I_stricmp(realname+nameLen-4,".mp3")==0;
    const auto probe=kisak::vita::VitaCpuAudio_Open(index-SND_FIRST_STREAM_CHANNEL,realname,stream->isMp3);
    if(!probe.valid)return SND_SetPlaybackIdNotPlayed(index);
    stream->generation=probe.generation;stream->channels=probe.channels;
    stream->sampleRate=probe.rate;stream->totalFrames=probe.frames;
    std::copy_n(streamPool[index-SND_FIRST_STREAM_CHANNEL],4,stream->buffers);
    std::copy_n(stream->buffers,4,stream->available);
    stream->bufferCount=stream->availableCount=4;
#else
    uint32_t openResult = (FS_FOpenFileReadStream(realname, &stream->fsHandle) & 0x80000000) == 0;
    if (!openResult)
    {
#if KISAK_VITA_BENCHMARK
        ++audioBenchmarkErrors;
#endif
        Com_PrintError(9, "Couldn't play stream '%s' from alias '%s' - file not found\n", realname, startAliasInfo->alias0->aliasName);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    size_t nameLen = strlen(filename);
    stream->isMp3 = nameLen >= 4 && I_stricmp(filename + nameLen - 4, ".mp3") == 0;

#if KISAK_VITA_BENCHMARK
    if (stream->benchmarkPcm) stream->isMp3=false;
#endif
    bool decoderOk = stream->isMp3
        ? drmp3_init(&stream->mp3, AL_StreamReadCallback, AL_Mp3SeekCallback, AL_Mp3TellCallback, NULL, stream, NULL)
        : drwav_init(&stream->wav, AL_StreamReadCallback, AL_WavSeekCallback, AL_WavTellCallback, stream, NULL);

    if (!decoderOk)
    {
        FS_FCloseFile(stream->fsHandle);
#if KISAK_VITA_BENCHMARK
        ++audioBenchmarkErrors;
#endif
        Com_PrintError(9, "Couldn't play stream '%s' from alias '%s' - invalid or corrupted format\n", realname, startAliasInfo->alias0->aliasName);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    stream->channels = stream->isMp3 ? stream->mp3.channels : stream->wav.channels;
    stream->sampleRate = stream->isMp3 ? stream->mp3.sampleRate : stream->wav.sampleRate;
#endif
    stream->looping = (startAliasInfo->alias0->flags & 1) != 0; // matches AIL_set_stream_loop_count's polarity above: flags bit 0 set == looping
    stream->active = true;
#if KISAK_VITA_BENCHMARK
    char streamDetail[192]{};
    std::snprintf(streamDetail,sizeof(streamDetail),"channel=%d hash=%u rate=%u channels=%u looping=%u eligible=%u pcm=%u suppressed=%u",
        index,stream->benchmarkNameHash,stream->sampleRate,stream->channels,unsigned(stream->looping),
        unsigned(stream->benchmarkEligible),unsigned(stream->benchmarkPcm),unsigned(stream->benchmarkSuppressed));
    kisak::vita::VitaScriptServicesTrace("map_audio_stream",filename,streamDetail);
#endif

    int srcChannelCount = stream->channels;
    int baserate = stream->sampleRate;

    // dr_mp3 doesn't know its total frame count up front the way dr_wav does from the RIFF
    // header - drmp3_get_pcm_frame_count does a one-time scan to find it, paid once here at
    // stream start (same spirit as the one-time cost of opening/parsing the file at all).
#if KISAK_VITA_CPU_RUNTIME
    const uint64_t totalFrames=stream->totalFrames;
#else
    uint64_t totalFrames = stream->isMp3
        ? drmp3_get_pcm_frame_count(&stream->mp3)
        : stream->wav.totalPCMFrameCount;
#endif

    ALuint source = alGlob.source[index];
    MSS_ApplyEqFilter(source, entchannel);

    float pitch = startAliasInfo->timescale ? startAliasInfo->pitch * (float)g_snd.timescale : startAliasInfo->pitch;
    alSourcef(source, AL_PITCH, pitch);

    // Duration at the pitch-adjusted rate, not the buffer's native rate - see the matching
    // comment in SND_StartAlias2DSample. Computed after `pitch` above is known, unlike the
    // loaded-sound versions, since streaming has no separate "set sample info" step to hang
    // this off of.
    float effectiveRate = baserate * pitch;
    int total_msec = effectiveRate > 0.0f ? (int)((int64_t)totalFrames * 1000 / effectiveRate) : 0;

    float realVolume = startAliasInfo->volume
        * g_snd.volume
        * g_snd.channelvol->channelvol[(startAliasInfo->alias0->flags & 0x3F00) >> 8].volume;
    if (g_snd.slaveLerp != 0.0 && !startAliasInfo->master && (startAliasInfo->alias0->flags & 4) != 0)
        realVolume = SND_GetLerpedSlavePercentage(startAliasInfo->alias0->slavePercentage) * realVolume;

    alSourcei(source, AL_LOOPING, AL_FALSE); // looping is handled by SND_FillStreamBuffers re-queuing, not AL_LOOPING
    SND_ApplyReverbSend(index, startAliasInfo->alias0);

    if (startAliasInfo->timeshift >= total_msec && total_msec != 0)
    {
        SND_ReleaseStreamChannel(index);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    if (total_msec == 0 && totalFrames == 0)
    {
        SND_ReleaseStreamChannel(index);
        Com_PrintError(1, "ERROR: Sound file '%s' is zero length, invalid\n", realname);
        return SND_SetPlaybackIdNotPlayed(index);
    }

    int start_msec;
    if (startAliasInfo->fraction == 0.0)
    {
        if (startAliasInfo->timeshift)
        {
            start_msec = startAliasInfo->timeshift;
        }
        else if ((startAliasInfo->alias0->flags & 0x20) != 0)
        {
            start_msec = SnapFloatToInt(random() * (float)total_msec) & 0xFFFFFF80;
        }
        else
        {
            start_msec = 0;
        }
    }
    else
    {
        start_msec = SnapFloatToInt((float)total_msec * startAliasInfo->fraction);
    }
    if (start_msec)
        startAliasInfo->startDelay = 0;

#if KISAK_VITA_CPU_RUNTIME
    const uint64_t targetFrame=start_msec>0 && baserate>0?uint64_t(start_msec)*baserate/1000:0;
    stream->framesQueued=targetFrame;
    if(!kisak::vita::VitaCpuAudio_Begin(index-SND_FIRST_STREAM_CHANNEL,stream->generation,targetFrame,stream->looping)) {
        SND_ReleaseStreamChannel(index);return SND_SetPlaybackIdNotPlayed(index);
    }
#else
    if (start_msec > 0 && baserate > 0)
    {
        uint64_t targetFrame = (uint64_t)start_msec * baserate / 1000;
        if (stream->isMp3)
            drmp3_seek_to_pcm_frame(&stream->mp3, targetFrame);
        else
            drwav_seek_to_pcm_frame(&stream->wav, targetFrame);
        stream->framesQueued = targetFrame; // SND_FillStreamBuffers adds onto this below
    }
#endif

    SND_FillStreamBuffers(index);

    if (!startAliasInfo->startDelay
        && (!g_snd.paused || !g_snd.pauseSettings[(startAliasInfo->alias0->flags & 0x3F00) >> 8]))
    {
        alSourcePlay(source);
    }

    int totalMsecForChan = total_msec + startAliasInfo->startDelay;
    if ((startAliasInfo->alias0->flags & 1) != 0)
        totalMsecForChan = 0;

    float *org = g_snd.chaninfo[index].org;
    *org = startAliasInfo->org[0];
    org[1] = startAliasInfo->org[1];
    org[2] = startAliasInfo->org[2];
    SND_SetChannelStartInfo(index, startAliasInfo);
    SND_SetSoundFileChannelInfo(index, srcChannelCount, baserate, totalMsecForChan, start_msec, SFLS_LOADED);

    if (SND_IsAliasChannel3D((g_snd.chaninfo[index].alias0->flags & 0x3F00) >> 8))
    {
        SND_GetCurrent3DPosition(g_snd.chaninfo[index].sndEnt, g_snd.chaninfo[index].offset, g_snd.chaninfo[index].org);
        int listenerIndex = SND_GetListenerIndexNearestToOrigin(g_snd.chaninfo[index].org);
        SND_Set3DStreamPosition(index, listenerIndex, g_snd.chaninfo[index].org);
        realVolume = (float)SND_GetStream3DVolumeFallOff(index, listenerIndex) * realVolume;
    }
    else
    {
        SND_ApplyChannelMap(source, startAliasInfo->alias0, srcChannelCount);
    }
    SND_SetStreamChannelVolume(index, realVolume);

    int playbackId = SND_AcquirePlaybackId(index, totalMsecForChan);
    if (playbackId != -1)
        SND_AddVoice(entchannel);
    return playbackId;
}

// snd_roomStrings[27] (snd.cpp) is the exact same 26-name, same-order I3DL2/EAX preset list
// as these - verified name-for-name, not assumed - so this is a mechanical 1:1 copy, not a
// design decision. SND_RoomtypeFromString (snd.cpp) maps a string to an index into this
// same list, so the two arrays must stay in lockstep.
static const EFXEAXREVERBPROPERTIES AL_RoomPresets[26] =
{
    EFX_REVERB_PRESET_GENERIC,
    EFX_REVERB_PRESET_PADDEDCELL,
    EFX_REVERB_PRESET_ROOM,
    EFX_REVERB_PRESET_BATHROOM,
    EFX_REVERB_PRESET_LIVINGROOM,
    EFX_REVERB_PRESET_STONEROOM,
    EFX_REVERB_PRESET_AUDITORIUM,
    EFX_REVERB_PRESET_CONCERTHALL,
    EFX_REVERB_PRESET_CAVE,
    EFX_REVERB_PRESET_ARENA,
    EFX_REVERB_PRESET_HANGAR,
    EFX_REVERB_PRESET_CARPETEDHALLWAY,
    EFX_REVERB_PRESET_HALLWAY,
    EFX_REVERB_PRESET_STONECORRIDOR,
    EFX_REVERB_PRESET_ALLEY,
    EFX_REVERB_PRESET_FOREST,
    EFX_REVERB_PRESET_CITY,
    EFX_REVERB_PRESET_MOUNTAINS,
    EFX_REVERB_PRESET_QUARRY,
    EFX_REVERB_PRESET_PLAIN,
    EFX_REVERB_PRESET_PARKINGLOT,
    EFX_REVERB_PRESET_SEWERPIPE,
    EFX_REVERB_PRESET_UNDERWATER,
    EFX_REVERB_PRESET_DRUGGED,
    EFX_REVERB_PRESET_DIZZY,
    EFX_REVERB_PRESET_PSYCHOTIC,
};

static void AL_ApplyReverbPreset(ALuint effect, const EFXEAXREVERBPROPERTIES &props)
{
    alEffectf(effect, AL_EAXREVERB_DENSITY, props.flDensity);
    alEffectf(effect, AL_EAXREVERB_DIFFUSION, props.flDiffusion);
    alEffectf(effect, AL_EAXREVERB_GAIN, props.flGain);
    alEffectf(effect, AL_EAXREVERB_GAINHF, props.flGainHF);
    alEffectf(effect, AL_EAXREVERB_GAINLF, props.flGainLF);
    alEffectf(effect, AL_EAXREVERB_DECAY_TIME, props.flDecayTime);
    alEffectf(effect, AL_EAXREVERB_DECAY_HFRATIO, props.flDecayHFRatio);
    alEffectf(effect, AL_EAXREVERB_DECAY_LFRATIO, props.flDecayLFRatio);
    alEffectf(effect, AL_EAXREVERB_REFLECTIONS_GAIN, props.flReflectionsGain);
    alEffectf(effect, AL_EAXREVERB_REFLECTIONS_DELAY, props.flReflectionsDelay);
    alEffectfv(effect, AL_EAXREVERB_REFLECTIONS_PAN, props.flReflectionsPan);
    alEffectf(effect, AL_EAXREVERB_LATE_REVERB_GAIN, props.flLateReverbGain);
    alEffectf(effect, AL_EAXREVERB_LATE_REVERB_DELAY, props.flLateReverbDelay);
    alEffectfv(effect, AL_EAXREVERB_LATE_REVERB_PAN, props.flLateReverbPan);
    alEffectf(effect, AL_EAXREVERB_ECHO_TIME, props.flEchoTime);
    alEffectf(effect, AL_EAXREVERB_ECHO_DEPTH, props.flEchoDepth);
    alEffectf(effect, AL_EAXREVERB_MODULATION_TIME, props.flModulationTime);
    alEffectf(effect, AL_EAXREVERB_MODULATION_DEPTH, props.flModulationDepth);
    alEffectf(effect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, props.flAirAbsorptionGainHF);
    alEffectf(effect, AL_EAXREVERB_HFREFERENCE, props.flHFReference);
    alEffectf(effect, AL_EAXREVERB_LFREFERENCE, props.flLFReference);
    alEffectf(effect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, props.flRoomRolloffFactor);
    alEffecti(effect, AL_EAXREVERB_DECAY_HFLIMIT, props.iDecayHFLimit);
}

// Sets the per-channel wet-send *gain* (AL_AUXILIARY_SEND_FILTER has no gain parameter of
// its own - see AlLocal::sendFilter's comment in snd_local.h for why a lowpass filter is
// used purely as a gain carrier here) and wires the channel's source to the global reverb
// aux slot. MSS_GetDryLevel() is hardcoded to 1.0f in the existing Miles code (dead code,
// see WORK.md Phase 6) so there's no dry-side control to port - the dry/direct path is left
// at its default (unfiltered, full AL_GAIN) which already matches "dry always 1.0".
void SND_ApplyReverbSend(int index, const snd_alias_t *alias)
{
    float wet = MSS_GetWetLevel(alias);
    alFilterf(alGlob.sendFilter[index], AL_LOWPASS_GAIN, wet);
    alFilterf(alGlob.sendFilter[index], AL_LOWPASS_GAINHF, 1.0f);
    alSource3i(alGlob.source[index], AL_AUXILIARY_SEND_FILTER, alGlob.auxSlot, 0, alGlob.sendFilter[index]);
}

void __cdecl SND_SetRoomtype(int roomtype)
{
    iassert(roomtype >= 0 && roomtype < ARRAY_COUNT(AL_RoomPresets));
    AL_ApplyReverbPreset(alGlob.reverbEffect, AL_RoomPresets[roomtype]);
    alAuxiliaryEffectSloti(alGlob.auxSlot, AL_EFFECTSLOT_EFFECT, alGlob.reverbEffect);
}

void __cdecl SND_UpdateEqs()
{
    // Mirrors the Miles loop, but MSS_ApplyEqFilter (snd_al.cpp) is a deliberate no-op
    for (int channelIndex = 0; channelIndex < 53; ++channelIndex)
    {
        MSS_ApplyEqFilter(alGlob.source[channelIndex], g_snd.chaninfo[channelIndex].entchannel);
    }
}

void __cdecl SND_SetEqParams(
    uint32_t entchannel,
    int eqIndex,
    uint32_t band,
    SND_EQTYPE type,
    float gain,
    float freq,
    float q)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(freq >= 0 && freq <= 20000);
    iassert(q > 0);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].gain = gain;
    alGlob.eq[eqIndex].params[band][entchannel].freq = freq;
    alGlob.eq[eqIndex].params[band][entchannel].q = q;
    alGlob.eq[eqIndex].params[band][entchannel].type = type;

#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
}

void __cdecl SND_SetEqType(uint32_t entchannel, int eqIndex, uint32_t band, SND_EQTYPE type)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].type = type;
}

void __cdecl SND_SetEqFreq(uint32_t entchannel, int eqIndex, uint32_t band, float freq)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(freq >= 0 && freq <= 20000);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].freq = freq;
}

void __cdecl SND_SetEqGain(uint32_t entchannel, int eqIndex, uint32_t band, float gain)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));
    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].gain = gain;
}

void __cdecl SND_SetEqQ(uint32_t entchannel, int eqIndex, uint32_t band, float q)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);
    iassert(q > 0);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 1;
    alGlob.eq[eqIndex].params[band][entchannel].q = q;

#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
}

void __cdecl SND_DisableEq(uint32_t entchannel, int eqIndex, uint32_t band)
{
    iassert(entchannel >= 0 && entchannel < 64);
    iassert(band >= 0 && band < 3);

    iassert((unsigned)eqIndex < ARRAY_COUNT(alGlob.eq));

    alGlob.eq[eqIndex].params[band][entchannel].enabled = 0;
}

void __cdecl SND_SaveEq(MemoryFile *memFile)
{
    int band; // [esp+0h] [ebp-Ch]
    int entchannel; // [esp+4h] [ebp-8h]
    int eqIndex; // [esp+8h] [ebp-4h]

    for (eqIndex = 0; eqIndex < 2; ++eqIndex)
    {
        for (band = 0; band < 3; ++band)
        {
            for (entchannel = 0; entchannel < 64; ++entchannel)
            {
                MemFile_WriteData(memFile, 20, &alGlob.eq[eqIndex].params[band][entchannel]);
            }
        }
    }
}

void __cdecl SND_RestoreEq(MemoryFile *memFile)
{
    int band; // [esp+0h] [ebp-Ch]
    int entchannel; // [esp+4h] [ebp-8h]
    int eqIndex; // [esp+8h] [ebp-4h]

    for (eqIndex = 0; eqIndex < 2; ++eqIndex)
    {
        for (band = 0; band < 3; ++band)
        {
            for (entchannel = 0; entchannel < 64; ++entchannel)
            {
                MemFile_ReadData(memFile, 20, (uint8_t *)&alGlob.eq[eqIndex].params[band][entchannel]);
            }
        }
    }
}

void __cdecl SND_PrintEqParams()
{
    float *v0; // edx
    snd_entchannel_info_t *channelName; // [esp+18h] [ebp-24h]
    int band; // [esp+1Ch] [ebp-20h]
    int entchannel; // [esp+20h] [ebp-1Ch]
    int eqIndex; // [esp+24h] [ebp-18h]

    Com_Printf(9, "Current EQ Settings\n---------------\n");
    for (entchannel = 0; entchannel < g_snd.entchannel_count; ++entchannel)
    {
        channelName = SND_GetEntChannelName(entchannel);
        Com_Printf(9, "+ %s\n", channelName->name);
        for (eqIndex = 0; eqIndex < 2; ++eqIndex)
        {
            for (band = 0; band < 3; ++band)
            {
                v0 = (float *)&alGlob.eq[eqIndex].params[band][entchannel];
                if ((uint8_t) * ((uint32_t *)v0 + 4))
                    Com_Printf(9, "\t%i %s %f Hz %f dB %f q\n", band, snd_eqTypeStrings[*(uint32_t *)v0], v0[2], v0[1], v0[3]);
            }
        }
    }
}

float __cdecl SND_Get2DChannelVolume(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    // Unlike Miles' separate left/right levels, AL_GAIN is a single scalar regardless of
    // channel count - no srcChannelCount branching needed on this side.
    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_Set2DChannelVolume(int index, float volume)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

float __cdecl SND_Get3DChannelVolume(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_Set3DChannelVolume(int index, float volume)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

float __cdecl SND_GetStreamChannelVolume(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_SetStreamChannelVolume(int index, float volume)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

int __cdecl SND_Get2DChannelPlaybackRate(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    ALfloat pitch;
    alGetSourcef(alGlob.source[index], AL_PITCH, &pitch);
    return SnapFloatToInt(pitch * g_snd.chaninfo[index].soundFileInfo.baserate);
}

void __cdecl SND_Set2DChannelPlaybackRate(int index, int rate)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);
    int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    alSourcef(alGlob.source[index], AL_PITCH, baserate ? (float)rate / baserate : 1.0f);
}

int __cdecl SND_Get3DChannelPlaybackRate(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALfloat pitch;
    alGetSourcef(alGlob.source[index], AL_PITCH, &pitch);
    return SnapFloatToInt(pitch * g_snd.chaninfo[index].soundFileInfo.baserate);
}

void __cdecl SND_Set3DChannelPlaybackRate(int index, int rate)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    alSourcef(alGlob.source[index], AL_PITCH, baserate ? (float)rate / baserate : 1.0f);
}

int __cdecl SND_GetStreamChannelPlaybackRate(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    // See SND_Get2DChannelPlaybackRate - same AL_PITCH-is-a-ratio conversion.
    ALfloat pitch;
    alGetSourcef(alGlob.source[index], AL_PITCH, &pitch);
    return SnapFloatToInt(pitch * g_snd.chaninfo[index].soundFileInfo.baserate);
}

void __cdecl SND_SetStreamChannelPlaybackRate(int index, int rate)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    alSourcef(alGlob.source[index], AL_PITCH, baserate ? (float)rate / baserate : 1.0f);
}

void __cdecl SND_Update2DChannelReverb(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    SND_ApplyReverbSend(index, g_snd.chaninfo[index].alias0);
}

void __cdecl SND_Update3DChannelReverb(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    SND_ApplyReverbSend(index, g_snd.chaninfo[index].alias0);
}

void __cdecl SND_UpdateStreamChannelReverb(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    SND_ApplyReverbSend(index, g_snd.chaninfo[index].alias0);
}

int __cdecl SND_Get2DChannelLength(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    return g_snd.chaninfo[index].totalMsec;
}

int __cdecl SND_Get3DChannelLength(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    return g_snd.chaninfo[index].totalMsec;
}

int __cdecl SND_GetStreamChannelLength(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    return g_snd.chaninfo[index].totalMsec;
}

void __cdecl SND_Get2DChannelSaveInfo(int index, snd_save_2D_sample_t *info)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    ALuint source = alGlob.source[index];
    ALfloat offsetSec = 0.0f;
    alGetSourcef(source, AL_SEC_OFFSET, &offsetSec);
    int totalMsec = g_snd.chaninfo[index].totalMsec;
    info->fraction = totalMsec > 0 ? (offsetSec * 1000.0f) / totalMsec : 0.0f;
    info->pitch = g_snd.chaninfo[index].pitch;

    ALfloat gain = 0.0f;
    alGetSourcef(source, AL_GAIN, &gain);
    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = gain / g_snd.volume;
}

void __cdecl SND_Set2DChannelFromSaveInfo(int index, snd_save_2D_sample_t *info)
{
    float volume; // [esp+4h] [ebp-4h]

    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    volume = info->volume * g_snd.volume;
    SND_Set2DChannelVolume(index, volume);
}

void __cdecl SND_Get3DChannelSaveInfo(int index, snd_save_3D_sample_t *info)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALuint source = alGlob.source[index];
    ALfloat offsetSec = 0.0f;
    alGetSourcef(source, AL_SEC_OFFSET, &offsetSec);
    int totalMsec = g_snd.chaninfo[index].totalMsec;
    info->fraction = totalMsec > 0 ? (offsetSec * 1000.0f) / totalMsec : 0.0f;
    info->pitch = g_snd.chaninfo[index].pitch;

    ALfloat gain = 0.0f;
    alGetSourcef(source, AL_GAIN, &gain);
    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = gain / g_snd.volume;

    alGetSource3f(source, AL_POSITION, &info->org[0], &info->org[1], &info->org[2]);
}

void __cdecl SND_GetStreamChannelSaveInfo(int index, snd_save_stream_t *info)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    AlStreamState *stream = SND_StreamState(index);
    int baserate = g_snd.chaninfo[index].soundFileInfo.baserate;
    int totalMsec = g_snd.chaninfo[index].totalMsec;
    if (totalMsec > 0 && baserate > 0)
    {
        uint64_t totalFrames = (uint64_t)totalMsec * baserate / 1000;
        info->fraction = totalFrames > 0 ? (float)stream->framesQueued / totalFrames : 0.0f;
    }
    else
    {
        info->fraction = 0.0f;
    }

    ALuint source = alGlob.source[index];
    ALfloat alPitch = 1.0f;
    alGetSourcef(source, AL_PITCH, &alPitch);
    int rate = SnapFloatToInt(alPitch * baserate);
    if (g_snd.chaninfo[index].timescale)
        rate = SnapFloatToInt((float)rate / g_snd.timescale);
    info->rate = rate;

    info->basevolume = g_snd.chaninfo[index].basevolume;
    ALfloat gain = 0.0f;
    alGetSourcef(source, AL_GAIN, &gain);
    if (g_snd.volume == 0.0)
        info->volume = g_snd.chaninfo[index].basevolume;
    else
        info->volume = gain / g_snd.volume;

    float *org = g_snd.chaninfo[index].org;
    info->org[0] = org[0];
    info->org[1] = org[1];
    info->org[2] = org[2];
}

void __cdecl SND_SetStreamChannelFromSaveInfo(int index, snd_save_stream_t *info)
{
    float volume; // [esp+4h] [ebp-4h]

    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    volume = info->volume * g_snd.volume;
    SND_SetStreamChannelVolume(index, volume);
}

int __cdecl SND_GetSoundFileSize(uint32_t *pSoundFile)
{
    iassert(pSoundFile);

    return pSoundFile[2];
}

void __cdecl SND_DriverPostUpdate()
{
#ifndef KISAK_XBOX
    SND_UpdateEqs();
#endif
    KISAK_NULLSUB();
}

void __cdecl SND_Update2DChannel(int i, int frametime)
{
    float v2; // [esp+4h] [ebp-18h]
    float volume; // [esp+8h] [ebp-14h]
    float volumea; // [esp+8h] [ebp-14h]
    const snd_alias_t *alias1; // [esp+Ch] [ebp-10h]
    const snd_alias_t *alias0; // [esp+10h] [ebp-Ch]
    snd_channel_info_t *chaninfo; // [esp+18h] [ebp-4h]

    iassert(i >= 0 && i < 0 + g_snd.max_2D_channels);

    chaninfo = &g_snd.chaninfo[i];
    if (!chaninfo->paused)
    {
        alias0 = chaninfo->alias0;
        alias1 = chaninfo->alias1;
        iassert(alias0);
        iassert(alias1);

        volume = chaninfo->basevolume;

        ALint state;
        alGetSourcei(alGlob.source[i], AL_SOURCE_STATE, &state);
        bool finishedOrChaining = (!chaninfo->startDelay && state == AL_STOPPED)
            || (alias0->chainAliasName && chaninfo->totalMsec + chaninfo->startTime - g_snd.time <= 0);
        if (finishedOrChaining)
        {
            SND_StopChannelAndPlayChainAlias(i);
        }
        else
        {
            if (g_snd.slaveLerp != 0.0 && !g_snd.chaninfo[i].master && (alias0->flags & 4) != 0)
                volume = SND_GetLerpedSlavePercentage(alias0->slavePercentage) * volume;
            iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);
            volumea = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
            v2 = volumea * g_snd.volume;
            SND_Set2DChannelVolume(i, v2);
            MSS_ResumeSample(i, frametime);
        }
    }
}

void __cdecl SND_Update3DChannel(int i, int frametime)
{
    double v2; // st7
    double LerpedSlavePercentage; // st7
    float v4; // [esp+Ch] [ebp-48h]
    float radius; // [esp+10h] [ebp-44h]
    snd_listener *a; // [esp+14h] [ebp-40h]
    float diff[3]; // [esp+1Ch] [ebp-38h] BYREF
    float volume; // [esp+28h] [ebp-2Ch]
    float lerp; // [esp+2Ch] [ebp-28h]
    const snd_alias_t *alias1; // [esp+30h] [ebp-24h]
    float distMin; // [esp+34h] [ebp-20h]
    const snd_alias_t *alias0; // [esp+38h] [ebp-1Ch]
    float org[3]; // [esp+3Ch] [ebp-18h] BYREF
    int timeleft; // [esp+48h] [ebp-Ch]
    float distMax; // [esp+4Ch] [ebp-8h]
    snd_channel_info_t *chaninfo; // [esp+50h] [ebp-4h]

    iassert(i >= (0 + 8) && i < (0 + 8) + g_snd.max_3D_channels);
    chaninfo = &g_snd.chaninfo[i];
    if (!chaninfo->paused)
    {
        alias0 = chaninfo->alias0;
        alias1 = chaninfo->alias1;
        iassert(alias0);
        iassert(alias1);
        lerp = chaninfo->lerp;
        volume = chaninfo->basevolume;
        timeleft = chaninfo->totalMsec + chaninfo->startTime - g_snd.time;
        ALint state;
        alGetSourcei(alGlob.source[i], AL_SOURCE_STATE, &state);
        bool finishedOrChaining = (!chaninfo->startDelay && state == AL_STOPPED)
            || (alias0->chainAliasName && timeleft <= 0);
        if (finishedOrChaining)
        {
            SND_StopChannelAndPlayChainAlias(i);
        }
        else
        {
            SND_GetCurrent3DPosition(g_snd.chaninfo[i].sndEnt, g_snd.chaninfo[i].offset, org);
            SND_Set3DPosition(i, org);
            distMin = (1.0 - lerp) * alias0->distMin + alias1->distMin * lerp;
            distMax = (1.0 - lerp) * alias0->distMax + alias1->distMax * lerp;
            a = &g_snd.listeners[SND_GetListenerIndexNearestToOrigin(org)];
            Vec3Sub(a->orient.origin, org, diff);
            radius = Vec3Length(diff);
            v2 = SND_Attenuate(alias0->volumeFalloffCurve, radius, distMin, distMax);
            volume = v2 * volume;
            if (g_snd.slaveLerp != 0.0 && !g_snd.chaninfo[i].master && (alias0->flags & 4) != 0)
            {
                LerpedSlavePercentage = SND_GetLerpedSlavePercentage(alias0->slavePercentage);
                volume = LerpedSlavePercentage * volume;
            }
            iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);
            volume = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
            v4 = volume * g_snd.volume;
            SND_Set3DChannelVolume(i, v4);
            MSS_ResumeSample(i, frametime);
        }
    }
}

void __cdecl SND_UpdateStreamChannel(int i, int frametime)
{
    int v2; // [esp+4h] [ebp-1Ch]
    float volume; // [esp+Ch] [ebp-14h]
    float volumea; // [esp+Ch] [ebp-14h]
    float volumeb; // [esp+Ch] [ebp-14h]
    int listenerIndex; // [esp+10h] [ebp-10h]
    const snd_alias_t *alias1; // [esp+14h] [ebp-Ch]
    const snd_alias_t *alias0; // [esp+18h] [ebp-8h]
    snd_channel_info_t *chaninfo; // [esp+1Ch] [ebp-4h]

    iassert(i >= ((0 + 8) + 32) && i < ((0 + 8) + 32) + g_snd.max_stream_channels);

#if KISAK_VITA_BENCHMARK
    ++audioPerf.updateCalls;
    if (SND_StreamState(i)->active && SND_StreamState(i)->benchmarkSuppressed) return;
#endif
    chaninfo = &g_snd.chaninfo[i];
    if (!chaninfo->paused && (i >= 45 || SND_UpdateBackgroundVolume(i - 40, frametime)))
    {
        alias0 = chaninfo->alias0;
        alias1 = chaninfo->alias1;
        iassert(alias0);
        iassert(alias1);
        volume = chaninfo->basevolume;
        bool hasMoreToPlay = SND_FillStreamBuffers(i);
        ALint state;
        alGetSourcei(alGlob.source[i], AL_SOURCE_STATE, &state);
#if KISAK_VITA_CPU_RUNTIME
        SND_ResumeExistingStream(i,hasMoreToPlay,state);
#endif
        bool stillPlaying = g_snd.chaninfo[i].startDelay || hasMoreToPlay || state != AL_STOPPED;
        if (stillPlaying)
        {
            if (SND_IsAliasChannel3D(SNDALIASFLAGS_GET_CHANNEL(alias0->flags)))
            {
                SND_GetCurrent3DPosition(g_snd.chaninfo[i].sndEnt, g_snd.chaninfo[i].offset, g_snd.chaninfo[i].org);
                listenerIndex = SND_GetListenerIndexNearestToOrigin(g_snd.chaninfo[i].org);
                SND_Set3DStreamPosition(i, listenerIndex, g_snd.chaninfo[i].org);
                volume = SND_GetStream3DVolumeFallOff(i, listenerIndex) * volume;
            }
            if (g_snd.slaveLerp != 0.0 && !g_snd.chaninfo[i].master && (alias0->flags & 4) != 0)
                volume = SND_GetLerpedSlavePercentage(alias0->slavePercentage) * volume;

            iassert(SNDALIASFLAGS_GET_CHANNEL(alias0->flags) < 64);

            volumea = volume * g_snd.channelvol->channelvol[(alias0->flags & 0x3F00) >> 8].volume;
            volumeb = volumea * g_snd.volume;
            SND_SetStreamChannelVolume(i, volumeb);
            if (g_snd.chaninfo[i].startDelay)
            {
                if (g_snd.chaninfo[i].startDelay - frametime > 0)
                    v2 = g_snd.chaninfo[i].startDelay - frametime;
                else
                    v2 = 0;
                g_snd.chaninfo[i].startDelay = v2;
                if (!g_snd.chaninfo[i].startDelay)
                {
                    alSourcePlay(alGlob.source[i]);
                }
            }
        }
        else
        {
            SND_StopChannelAndPlayChainAlias(i);
        }
    }
}

void __cdecl SND_SetHWND(HWND hwnd)
{
    // OpenAL has no window-handle dependency (no DirectSound backend to bind).
}

void __cdecl SND_SetData(MssSoundCOD4 *mssSound, void *srcData)
{
    // dr_wav (see SND_LoadFromBuffer, snd_driver_load_obj.cpp) always decodes to 16-bit PCM
    // for us, so unlike the Miles branch above there's no ADPCM format to worry about here.
    if (mssSound->info.rate > g_snd.playback_rate)
    {
        // Resample down to g_snd.playback_rate via simple decimation (nearest-frame
        // resample), matching the halving loop in the Miles branch above. A real
        // low-pass-filtered resample would sound better, but this matches WORK.md Phase 3's
        // stated scope - revisit if downsampled loaded sounds turn out to sound too aliased.
        uint32_t srcFrameCount = mssSound->info.samples;
        uint32_t channels = mssSound->info.channels;
        uint32_t rate = mssSound->info.rate;
        uint32_t frameCount = srcFrameCount;

        while (rate > g_snd.playback_rate)
        {
            rate /= 2;
            frameCount /= 2;
        }

        uint32_t newDataLen = frameCount * channels * sizeof(int16_t);
        mssSound->data = MSS_Alloc(newDataLen, rate);

        const int16_t *src16 = (const int16_t *)srcData;
        int16_t *dst16 = (int16_t *)mssSound->data;
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            uint32_t srcFrame = (uint32_t)((uint64_t)i * srcFrameCount / frameCount);
            for (uint32_t c = 0; c < channels; ++c)
                dst16[i * channels + c] = src16[srcFrame * channels + c];
        }

        mssSound->info.rate = rate;
        mssSound->info.samples = frameCount;
        mssSound->info.data_len = newDataLen;
    }
    else
    {
        mssSound->data = MSS_Alloc(mssSound->info.data_len, mssSound->info.rate);
        Com_Memcpy(mssSound->data, srcData, mssSound->info.data_len);
    }

    mssSound->info.data_ptr = mssSound->data;
    mssSound->info.initial_ptr = mssSound->data;
}

#ifdef KISAK_SP
void SND_SetEqLerp(double lerp)
{
    if (lerp < 0.0 || lerp > 1.0)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\xenon\\snd_driver.cpp",
            1740,
            0,
            "%s\n\t(lerp) = %g",
            HIDWORD(lerp),
            LODWORD(lerp));
#if KISAK_XBOX
    xaGlob.eqLerp = lerp;
#else
    alGlob.eqLerp = (float)lerp;
    SND_UpdateEqs();
#endif
}
#endif











#endif
