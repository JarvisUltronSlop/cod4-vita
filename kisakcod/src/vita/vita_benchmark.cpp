#include <vita/vita_benchmark.h>
#include <algorithm>
#include <vita/vita_benchmark_metrics.h>
VitaBenchmarkMetrics vitaBenchmarkMetrics;
#include <cstring>
#include <vita/vita_benchmark_audio_inputs.h>

static unsigned benchmarkSeed = 0;
extern "C" unsigned VitaBenchmark_SourceSeed() { return benchmarkSeed; }
unsigned VitaBenchmark_SourceSeedForGame() { return benchmarkSeed; }
namespace kisak::vita {
bool VitaBenchmark::Append(const char* line) {
    const size_t n = std::strlen(line);
    if (!file_ || bytes_ + n > 64u * 1024u * 1024u ||
        std::fwrite(line, 1, n, file_) != n) {
        valid_=false; active_=false; benchmarkSeed=0;
        std::snprintf(message_, sizeof(message_), "BENCHMARK LOG WRITE FAILED - COPY LOGS");
        return false;
    }
    bytes_ += n;
    return true;
}
bool VitaBenchmark::Start(const char* path, const char* device, const char* contentHash, uint32_t round) {
    if (active_ || (round != 1 && round != 2 && round != 3 && round != 4 && round != 5 && round != 6 && round != 7 && round != 8 && round != 9)) return false;
    round_=round;
    if (file_) std::fclose(file_);
    file_=std::fopen(path, "wb");
    if (!file_) { std::snprintf(message_,sizeof(message_),"CANNOT OPEN BENCHMARK LOG"); return false; }
    std::setvbuf(file_, buffer_.data(), _IOFBF, buffer_.size());
    index_=count_=stalls_=identityIndex_=0; bytes_=0;
    previousBlockedInput_={}; blockedInputFrames_=blockedInputEvents_=blockedInputRecords_=0;
    active_=valid_=true; needsLoad_=measuring_=false; benchmarkSeed=kSeed;
    std::snprintf(path_,sizeof(path_),"%s",path);
    std::snprintf(message_,sizeof(message_),"PREPARING BENCHMARK IDENTITIES");
    char line[2048];
    std::snprintf(line,sizeof(line),
        "{\"type\":\"run\",\"schema\":1,\"round\":\"%s\",\"scenario\":\"stationary-neutral-spawn-v1\",\"inputPolicy\":\"blocked-gameplay-v1\",\"seed\":%u,\"device\":\"%s\",\"sourceInstallHash\":\"%s\",\"display\":[960,544],\"post\":[960,544],\"hud\":\"native-after-post\",\"warmupUs\":10000000,\"minimumUs\":60000000,\"minimumFrames\":40,\"maximumUs\":120000000,\"cpuUtilization\":null,\"gpuUtilization\":null,\"temperature\":null,\"shaderCacheState\":\"not-measured\",\"shaderCompiles\":null,\"audioOutputWaitUs\":null,\"audioUnderruns\":null,\"stockClockVerified\":false,\"hardwareQualified\":false,\"recorderBytes\":%u}\n",
        RoundName(),kSeed,device,contentHash,unsigned(sizeof(*this)));
    return Append(line);
}
void VitaBenchmark::RecordBlockedInput(const std::array<int32_t,8>& activity, bool edge) {
    const bool present=edge || std::any_of(activity.begin(),activity.end(),[](int32_t value){return value!=0;});
    if (present) {
        ++blockedInputFrames_;
        if (edge || activity!=previousBlockedInput_) {
            ++blockedInputEvents_;
            // Bounded edge diagnostics; persistent totals still cover the run.
            if (blockedInputRecords_ < 32) {
                ++blockedInputRecords_;
                char line[512];
                std::snprintf(line,sizeof(line),
                    "{\"type\":\"blocked-input\",\"case\":%u,\"measuring\":%s,\"deliveredToSource\":false,\"gameplayButtons\":%u,\"padButtons\":%u,\"move\":[%d,%d],\"look\":[%d,%d],\"rearFlags\":%u,\"touchReports\":%u,\"edge\":%s}\n",
                    index_,measuring_?"true":"false",uint32_t(activity[0]),uint32_t(activity[1]),
                    activity[2],activity[3],activity[4],activity[5],uint32_t(activity[6]),uint32_t(activity[7]),edge?"true":"false");
                Append(line);
            }
        }
    }
    previousBlockedInput_=activity;
}
void VitaBenchmark::Prepare() {
    static const char* paths[] = {"app0:eboot.bin", "app0:benchmark-identity.json",
        "ux0:data/CODVIT001/paks/common.kvap", "ux0:data/CODVIT001/paks/mp_shipment.kvap",
        "ux0:data/CODVIT001/paks/mp_crossfire.kvap"};
    const size_t totalPaths=std::size(paths)+(round_==6?std::size(kAudioBenchmarkIdentityPaths):0);
    if (!active_ || identityIndex_ >= totalPaths) return;
    const char* path=identityIndex_<std::size(paths)?paths[identityIndex_]
        :kAudioBenchmarkIdentityPaths[identityIndex_-std::size(paths)];
    if (!identityFile_) {
        identityFile_=std::fopen(path,"rb"); identityHash_={};
        if (!identityFile_) { Abort("identity-file-missing"); return; }
    }
    // One bounded read per displayed menu frame; no full-pack blocking hash.
    const size_t n=std::fread(identityBuffer_.data(),1,identityBuffer_.size(),identityFile_);
    identityHash_.Update(identityBuffer_.data(),n);
    if (std::ferror(identityFile_)) { Abort("identity-read-failed"); return; }
    if (!std::feof(identityFile_)) return;
    std::fclose(identityFile_); identityFile_=nullptr;
    const auto hash=identityHash_.Finish(); char hex[65]{};
    for (unsigned i=0;i<32;++i) std::snprintf(hex+2*i,3,"%02x",unsigned(hash[i]));
    char line[384]; std::snprintf(line,sizeof(line),
        "{\"type\":\"identity\",\"path\":\"%s\",\"sha256\":\"%s\"}\n",path,hex);
    Append(line);
    if (++identityIndex_ == totalPaths) needsLoad_=true;
}
void VitaBenchmark::TextureUpload(uint32_t nameHash, VitaTextureRole role, uint32_t width,
    uint32_t height, uint32_t levels, uint32_t format, uint32_t flags, size_t bytes,
    const VitaTextureMipSelection& selected, bool success) {
    if (!active_ || round_!=7) return;
    char line[768];
    std::snprintf(line,sizeof(line),
        "{\"type\":\"texture-upload\",\"case\":%u,\"mode\":%u,\"nameHash\":%u,\"role\":%u,\"source\":[%u,%u,%u],\"format\":%u,\"flags\":%u,\"sourceBytes\":%llu,\"selected\":[%u,%u,%u],\"offset\":%llu,\"selectedBytes\":%llu,\"valid\":%s,\"reduced\":%s,\"success\":%s}\n",
        index_,Case().textureMode,nameHash,uint32_t(role),width,height,levels,format,flags,
        (unsigned long long)bytes,selected.width,selected.height,selected.levels,
        (unsigned long long)selected.offset,(unsigned long long)selected.bytes,
        selected.valid?"true":"false",selected.reduced?"true":"false",success?"true":"false");
    Append(line);
}
void VitaBenchmark::Loaded(uint64_t now) {
    loadedAt_=now; firstEnd_=lastEnd_=duration_=0; count_=stalls_=0;
    needsLoad_=false; measuring_=false;
    std::snprintf(message_,sizeof(message_),"%u/%u %s - WARMUP",index_+1,CaseCount(),Case().id);
    char line[512]; std::snprintf(line,sizeof(line),
        "{\"type\":\"case\",\"index\":%u,\"id\":\"%s\",\"baseline\":\"%s\",\"factor\":\"%s\",\"scene\":[%u,%u],\"map\":0,\"sourceReset\":true,\"clearOnly\":%s,\"shaderMode\":%u,\"geometryMode\":%u,\"audioMode\":%u,\"textureMode\":%u}\n",
        index_,Case().id,Case().baseline,Case().factor,Case().width,Case().height,Case().clearOnly?"true":"false",Case().shaderMode,Case().geometryMode,Case().audioMode,Case().textureMode);
    Append(line);
}
bool VitaBenchmark::Record(const VitaBenchmarkFrame& b) {
    if (!active_ || needsLoad_ || !loadedAt_) return false;
    const auto& f=b.frame;
    if (f.wallEndUsec < loadedAt_ || (lastEnd_ && f.wallEndUsec <= lastEnd_)) {
        Abort("clock-discontinuity"); return false;
    }
    if (!measuring_) {
        if (f.wallEndUsec-loadedAt_ < 10000000u) return false;
        measuring_=true; firstEnd_=lastEnd_=f.wallEndUsec;
        std::snprintf(message_,sizeof(message_),"%u/%u %s - MEASURING",index_+1,CaseCount(),Case().id);
        return false; // Do not measure the warmup/measurement boundary interval.
    }
    const uint64_t interval=f.wallEndUsec-lastEnd_; lastEnd_=f.wallEndUsec;
    if (count_ == times_.size() || interval > UINT32_MAX) { Abort("sample-overflow"); return false; }
    times_[count_++]=uint32_t(interval); duration_=f.wallEndUsec-firstEnd_;
    if (interval>250000) ++stalls_;
    char line[6144];
    char cpuBytes[32]="null", gpuBytes[32]="null";
    if(f.cpuMemoryAvailable) std::snprintf(cpuBytes,sizeof(cpuBytes),"%llu",(unsigned long long)f.cpuResidentBytes);
    if(f.gpuMemoryAvailable) std::snprintf(gpuBytes,sizeof(gpuBytes),"%llu",(unsigned long long)f.gpuResidentBytes);
    const uint64_t accounted=uint64_t(f.sourceServerUsec)+f.sourceClientUsec+f.audioUsec+b.loopbackUs+b.renderUs;
    // Rendering contains post/UI/submission/swap; audio decode contains file
    // reads. All nested intervals are named and must not be added again.
    std::snprintf(line,sizeof(line),
        "{\"type\":\"frame\",\"case\":%u,\"n\":%u,\"endUs\":%llu,\"intervalUs\":%llu,\"mainElapsedUs\":%u,\"otherMainUs\":%llu,\"accountingValid\":%s,\"serverUs\":%u,\"clientUs\":%u,\"loopbackUs\":%u,\"audioUs\":%u,\"renderUs\":%u,\"visibilityUs\":%u,\"worldSubmitUs\":%u,\"postUs\":%u,\"uiUs\":%u,\"swapUs\":%u,\"sceneEndUs\":%u,\"queueUs\":%u,\"gcWaitUs\":%u,\"gcWorkerAsyncUs\":%u,\"swapOtherUs\":%u,\"gcFrees\":%u,\"gcTargets\":%u,\"ticks\":%u,\"backlogMs\":%u,\"clFrameUs\":%u,\"cgameUs\":%u,\"draws\":%u,\"triangles\":%u,\"states\":%u,\"transientBytes\":%u,\"transientDrops\":%u,\"shadowStateUpdates\":%u,\"lights\":%u,\"scene\":[%u,%u],\"textureBytes\":%llu,\"meshBytes\":%llu,\"targetLogicalBytes\":%llu,\"cpuMemoryAvailable\":%s,\"gpuMemoryAvailable\":%s,\"cpuResidentBytes\":%s,\"gpuResidentBytes\":%s,\"audioReadUs\":%llu,\"audioReadBytes\":%llu,\"audioDecodeIncludingReadUs\":%llu,\"audioFrames\":%llu,\"audioUploadUs\":%llu,\"audioQueueQueryUs\":%llu,\"audioStreams\":%u,\"clocks\":[%d,%d,%d,%d],\"battery\":%d,\"charging\":%d,\"origin\":[%.4f,%.4f,%.4f],\"angles\":[%.4f,%.4f,%.4f],\"weapon\":%u,\"sourceTime\":%u,\"worldRenderUs\":%u,\"propsFxRenderUs\":%u,\"viewmodelRenderUs\":%u,\"priorRecorderUs\":%u,\"priorPacingUs\":%u,\"systemAppMode\":%u,\"msaaEnum\":%u,\"displayBuffers\":%u,\"sunOnlyShader\":%s,\"bloomActive\":%s,\"snapshotsUs\":%llu,\"predictionUs\":%llu,\"entitiesUs\":%llu,\"cameraUs\":%llu,\"predictedCommands\":%u,\"viewmodelAnimationUs\":%llu,\"shaderMode\":%u,\"sceneShaderDraws\":%u,\"simpleMaterialDraws\":%u,\"postCopyDraws\":%u,\"authoredPostDraws\":%u,\"noLocalLightDraws\":%u,\"noSpecularDraws\":%u,\"specularEligibleDraws\":%u,\"nonSunLightInputs\":%u,\"noNormalMapDraws\":%u,\"normalEligibleDraws\":%u,\"adaptiveFullLightDraws\":%u,\"geometryMode\":%u,\"staticArchivePasses\":%u,\"staticDrawFailures\":%u,\"staticModelUs\":%u,\"worldGeometry\":[%u,%u],\"viewmodelGeometry\":[%u,%u],\"staticCandidates\":[%u,%u],\"staticCandidateTriangles\":[%u,%u],\"staticCandidateInstances\":[%u,%u],\"staticSkipped\":[%u,%u],\"staticSkippedTriangles\":[%u,%u],\"staticSubmitted\":[%u,%u],\"staticSubmittedTriangles\":[%u,%u],\"audioMode\":%u,\"audioErrors\":%u,\"eligibleStreams\":%u,\"pcmStreams\":%u,\"suppressedStreams\":%u,\"streamUpdateCalls\":%u}\n",
        index_,count_,(unsigned long long)f.wallEndUsec,(unsigned long long)interval,f.cpuWorkUsec,
        (unsigned long long)(accounted<=f.cpuWorkUsec?f.cpuWorkUsec-accounted:0),accounted<=f.cpuWorkUsec?"true":"false",
        f.sourceServerUsec,f.sourceClientUsec,b.loopbackUs,f.audioUsec,b.renderUs,f.visibilityUsec,f.drawSubmitUsec,b.postUs,b.uiUs,
        f.swapWaitUsec,b.sceneEndUs,b.queueUs,b.gcWaitUs,b.gcWorkerUs,b.swapOtherUs,b.gcFrees,b.gcTargets,b.serverTicks,b.backlogMs,b.clFrameUs,b.cgameUs,
        f.drawCalls,f.triangles,f.stateChanges,b.transientBytes,b.transientDrops,b.shadowPasses,b.lightCount,b.sceneWidth,b.sceneHeight,
        (unsigned long long)b.textureBytes,(unsigned long long)b.meshBytes,(unsigned long long)b.targetBytes,
        f.cpuMemoryAvailable?"true":"false",f.gpuMemoryAvailable?"true":"false",cpuBytes,gpuBytes,
        (unsigned long long)b.audioReadUs,(unsigned long long)b.audioReadBytes,(unsigned long long)b.audioDecodeUs,(unsigned long long)b.audioFrames,
        (unsigned long long)b.audioUploadUs,(unsigned long long)b.audioQueueUs,b.audioStreams,b.arm,b.bus,b.gpu,b.xbar,b.battery,b.charging,
        b.origin[0],b.origin[1],b.origin[2],b.angles[0],b.angles[1],b.angles[2],b.weapon,b.sourceTime,b.worldUs,b.propsFxUs,b.viewmodelDrawUs,b.priorRecorderUs,b.priorPacingUs,b.sysAppMode,b.msaaEnum,b.displayBuffers,
        b.sunOnlyShader?"true":"false",b.bloomActive?"true":"false",(unsigned long long)b.clientPhases.snapshotsUs,
        (unsigned long long)b.clientPhases.predictionUs,(unsigned long long)b.clientPhases.entitiesUs,(unsigned long long)b.clientPhases.cameraUs,b.clientPhases.predictedCommands,(unsigned long long)b.clientPhases.viewmodelUs,b.shaderMode,b.sceneShaderDraws,b.simpleMaterialDraws,b.postCopyDraws,b.authoredPostDraws,b.noLocalLightDraws,b.noSpecularDraws,b.specularEligibleDraws,b.nonSunLightInputs,b.noNormalMapDraws,b.normalEligibleDraws,b.adaptiveFullLightDraws,
        b.geometry.mode,b.geometry.archivePasses,b.geometry.failures,b.geometry.staticModelUs,
        b.geometry.world[0],b.geometry.world[1],b.geometry.viewmodel[0],b.geometry.viewmodel[1],
        b.geometry.candidates[0],b.geometry.candidates[1],
        b.geometry.candidateTriangles[0],b.geometry.candidateTriangles[1],
        b.geometry.candidateInstances[0],b.geometry.candidateInstances[1],
        b.geometry.skipped[0],b.geometry.skipped[1],
        b.geometry.skippedTriangles[0],b.geometry.skippedTriangles[1],
        b.geometry.submitted[0],b.geometry.submitted[1],
        b.geometry.submittedTriangles[0],b.geometry.submittedTriangles[1],
        b.audioDetail.mode,b.audioDetail.errors,b.audioDetail.eligibleStreams,b.audioDetail.pcmStreams,
        b.audioDetail.suppressedStreams,b.audioDetail.updateCalls);
    if (!Append(line)) return false;
    if(round_==9) {
        std::snprintf(line,sizeof(line),
            "{\"type\":\"balanced-frame\",\"case\":%u,\"n\":%u,\"world\":[%u,%u],\"viewmodel\":[%u,%u],"
            "\"compositionUs\":%u,\"grassOptionalCandidates\":%u,\"grassOmissions\":%u,\"balancedGrass\":%s,"
            "\"adaptiveLighting\":%s,\"targetLogicalBytes\":%llu}\n",
            index_,count_,f.worldWidth,f.worldHeight,f.viewmodelWidth,f.viewmodelHeight,
            f.compositionUsec,f.grassCandidates,f.grassOmissions,f.balancedGrass?"true":"false",
            f.adaptiveLighting?"true":"false",(unsigned long long)f.targetLogicalBytes);
        if(!Append(line)) return false;
    }
    if (round_==8) {
        const auto& d=b.debris;
        std::snprintf(line,sizeof(line),
            "{\"type\":\"debris-fog-frame\",\"case\":%u,\"n\":%u,\"debrisMode\":%u,\"failures\":%u,\"noFogDraws\":%u,\"fogEligibleDraws\":%u,\"fogEnabled\":%s,\"fogStart\":%.9g,\"fogDensity\":%.9g,\"textureMode\":%u}\n",
            index_,count_,d.mode,d.failures,b.noFogDraws,b.fogEligibleDraws,
            b.fogEnabled?"true":"false",b.fogStart,b.fogDensity,b.textures.mode);
        if (!Append(line)) return false;
        for (unsigned c=0;c<4;++c) {
            std::snprintf(line,sizeof(line),
                "{\"type\":\"debris-category-frame\",\"case\":%u,\"n\":%u,\"category\":%u,\"candidates\":%u,\"candidateTriangles\":%u,\"candidateInstances\":%u,\"skipped\":%u,\"skippedTriangles\":%u,\"submitted\":%u,\"submittedTriangles\":%u}\n",
                index_,count_,c,d.candidates[c],d.candidateTriangles[c],d.candidateInstances[c],
                d.skipped[c],d.skippedTriangles[c],d.submitted[c],d.submittedTriangles[c]);
            if (!Append(line)) return false;
        }
    }
    if (round_==7) {
        const auto& t=b.textures;
        std::snprintf(line,sizeof(line),
            "{\"type\":\"texture-frame\",\"case\":%u,\"n\":%u,\"mode\":%u,\"failures\":%u,\"attempts\":[%u,%u,%u],\"reduced\":[%u,%u,%u],\"sourceBytes\":[%llu,%llu,%llu],\"selectedBytes\":[%llu,%llu,%llu]}\n",
            index_,count_,t.mode,t.failures,t.attempts[0],t.attempts[1],t.attempts[2],
            t.reduced[0],t.reduced[1],t.reduced[2],
            (unsigned long long)t.sourceBytes[0],(unsigned long long)t.sourceBytes[1],(unsigned long long)t.sourceBytes[2],
            (unsigned long long)t.selectedBytes[0],(unsigned long long)t.selectedBytes[1],(unsigned long long)t.selectedBytes[2]);
        if (!Append(line)) return false;
    }
    if (round_==6) for (unsigned channel=0;channel<13;++channel) {
        const auto& a=b.audioDetail.streams[channel];
        if (!a.active) continue;
        std::snprintf(line,sizeof(line),
            "{\"type\":\"audio-stream-frame\",\"case\":%u,\"n\":%u,\"channel\":%u,\"nameHash\":%u,\"rate\":%u,\"channels\":%u,\"eligible\":%s,\"pcm\":%s,\"suppressed\":%s,\"readUs\":%llu,\"readBytes\":%llu,\"readCalls\":%u,\"decodeIncludingReadUs\":%llu,\"decodedFrames\":%llu,\"refillCalls\":%u,\"processedBuffers\":%u,\"uploads\":%u,\"stoppedWithQueuedBuffers\":%u}\n",
            index_,count_,channel+40,a.nameHash,a.rate,a.channels,a.eligible?"true":"false",
            a.pcm?"true":"false",a.suppressed?"true":"false",(unsigned long long)a.readUs,
            (unsigned long long)a.readBytes,a.readCalls,(unsigned long long)a.decodeUs,
            (unsigned long long)a.frames,a.refillCalls,a.processedBuffers,a.uploads,a.stoppedQueued);
        if (!Append(line)) return false;
    }
    const uint64_t minimum=Case().clearOnly?10000000u:60000000u;
    const uint64_t maximum=Case().clearOnly?30000000u:120000000u;
    const uint32_t minimumFrames=Case().clearOnly?10u:40u;
    if ((duration_>=minimum && count_>=minimumFrames) || duration_>=maximum) {
        valid_=count_>=minimumFrames;
        FinishSegment();
        if (!active_) return false;
        if (++index_ == CaseCount()) {
            index_=CaseCount()-1; active_=false; benchmarkSeed=0;
            char end[256]; std::snprintf(end,sizeof(end),
                "{\"type\":\"end\",\"allSegmentsWritten\":true,\"hardwareQualified\":false,\"blockedInputFrames\":%u,\"blockedInputEvents\":%u,\"blockedInputRecords\":%u}\n",
                blockedInputFrames_,blockedInputEvents_,blockedInputRecords_);
            Append(end);
            if (std::fclose(file_)!=0) std::snprintf(message_,sizeof(message_),"LOG CLOSE FAILED - COPY LOGS");
            else std::snprintf(message_,sizeof(message_),"ROUND COMPLETE - COPY logs/bench-*.jsonl");
            file_=nullptr; return false;
        }
        needsLoad_=true; return true;
    }
    return false;
}
void VitaBenchmark::FinishSegment() {
    std::sort(times_.begin(),times_.begin()+count_);
    char line[512]; std::snprintf(line,sizeof(line),
        "{\"type\":\"result\",\"case\":%u,\"samples\":%u,\"durationUs\":%llu,\"medianUs\":%u,\"p95Us\":%u,\"maxUs\":%u,\"stalls\":%u,\"complete\":%s}\n",
        index_,count_,(unsigned long long)duration_,times_[(count_-1)/2],times_[(95*count_+99)/100-1],times_[count_-1],stalls_,valid_?"true":"false");
    Append(line);
    if (file_ && std::fflush(file_)!=0) Abort("log-flush-failed");
}
void VitaBenchmark::Abort(const char* reason) {
    if (identityFile_) { std::fclose(identityFile_); identityFile_=nullptr; }
    if (file_) {
        char line[256]; std::snprintf(line,sizeof(line),"{\"type\":\"abort\",\"reason\":\"%s\"}\n",reason);
        Append(line); std::fclose(file_); file_=nullptr;
    }
    active_=false; benchmarkSeed=0;
    std::snprintf(message_,sizeof(message_),"BENCHMARK STOPPED: %s",reason);
}
VitaBenchmark::~VitaBenchmark() { if (active_) Abort("shutdown"); else if(file_) std::fclose(file_); }
}
