#include <vita/vita_benchmark.h>
#include <vita/vita_benchmark_audio.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
using namespace kisak::vita;
static VitaBenchmark bench;
struct TestInput {
    uint32_t buttons=0;
    int8_t forwardmove=0,rightmove=0,lookX=0,lookY=0;
    float lookDegrees[2]{};
    int32_t viewAngles[2]{};
    bool menuBackPressed=false,startPressed=false;
    bool rearFragHeld=false,rearSpecialHeld=false,rearSprintHeld=false,rearMeleePressed=false;
    bool weaponCyclePressed=false,performanceTogglePressed=false,selectPressed=false;
};
static void Require(bool ok,const char* message) {
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
int main() {
    Require(VitaBenchmarkBackgroundStream("ambient\\amb_middle_east1v7_lr.mp3",true),"source backslash delimiter supported");
    Require(VitaBenchmarkPcmStream("ambient\\amb_middle_east1v7_lr.mp3"),"PCM maps source filesystem spelling");
    Require(VitaBenchmarkAudioNameHash("AMBIENT\\amb_middle_east1v7_lr.mp3")==VitaBenchmarkAudioNameHash("ambient/amb_middle_east1v7_lr.mp3"),"canonical identity follows Vita filesystem");
    Require(VitaBenchmarkBackgroundStream("ambient/loop.mp3",true),"looped background eligible");
    Require(!VitaBenchmarkBackgroundStream("weapons/fire.wav",true),"combat cues excluded");
    Require(!VitaBenchmarkBackgroundStream("music/spawn.mp3",false),"one-shot lifecycle unchanged");
    Require(VitaBenchmarkPcmStream("ambient/amb_middle_east1v7_lr.mp3"),"exact PCM source selected");
    Require(!VitaBenchmarkPcmStream("music/spawn.mp3"),"other MP3 stays original");
    Require(VitaBenchmarkFoliageModel("foliage_drygrass_squareclump"),"source grass category");
    Require(VitaBenchmarkBackgroundStream("ambient/loop.mp3",true),"looped background eligible");
    Require(!VitaBenchmarkBackgroundStream("weapons/fire.wav",true),"combat cues excluded");
    Require(!VitaBenchmarkBackgroundStream("music/spawn.mp3",false),"one-shot lifecycle unchanged");
    Require(VitaBenchmarkPcmStream("ambient/amb_middle_east1v7_lr.mp3"),"exact PCM source selected");
    Require(!VitaBenchmarkPcmStream("music/spawn.mp3"),"other MP3 stays original");
    Require(VitaBenchmarkFoliageModel("foliage_tree_pine_xl_b"),"source tree category");
    Require(!VitaBenchmarkFoliageModel("wc/me_ground_drygrass_dec"),"ground texture retained");
    Require(!VitaBenchmarkFoliageModel("prop_fence"),"fence retained");
    for (unsigned mode=0;mode<3;++mode) {
        VitaBenchmarkGeometry g{}; g.mode=mode;
        for (bool foliage : {true,false}) {
            const bool visible=g.Admit(foliage,18,7);
            Require(visible==!(mode==2 || (mode==1 && foliage)),"only selected category suppressed");
            if(visible) g.Submitted(foliage,18,7,true);
        }
        for (unsigned i=0;i<2;++i) {
            Require(g.candidateTriangles[i]==42 && g.candidateInstances[i]==7,"instanced expansion counted");
            Require(g.skipped[i]+g.submitted[i]==g.candidates[i],"batch conservation");
            Require(g.skippedTriangles[i]+g.submittedTriangles[i]==g.candidateTriangles[i],"triangle conservation");
        }
        g.Submitted(false,18,7,false); Require(g.failures==1,"submission failures observable");
    }
    Require(bench.Start("benchmark-test.jsonl","test","abc"),"open");
    uint64_t clock=1;
    for (unsigned c=0;c<6;++c) {
        Require(bench.Index()==c,"ordered restored baselines");
        bench.Loaded(clock);
        VitaBenchmarkFrame frame{};
        clock+=10000000; frame.frame.wallEndUsec=clock;
        Require(!bench.Record(frame),"warmup boundary discarded");
        for (unsigned n=0;n<(c==0?10:60);++n) {
            clock+=1000000; frame.frame.wallEndUsec=clock;
            bench.Record(frame);
        }
        Require(c==5 ? !bench.Active() : bench.NeedsLoad(),"measurement completes, requests source reset");
    }
    Require(VitaBenchmark_SourceSeed()==0,"seed restored after completion");
    std::ifstream f("benchmark-test.jsonl"); std::stringstream all; all<<f.rdbuf();
    Require(all.str().find("\"allSegmentsWritten\":true")!=std::string::npos,"completion recorded");
    Require(bench.Start("benchmark-timeout.jsonl","test","abc"),"restart");
    // Clear control reaches its cap with too few frames: incomplete, not a pass.
    bench.Loaded(1); VitaBenchmarkFrame sample{};
    sample.frame.wallEndUsec=10000001; bench.Record(sample);
    sample.frame.wallEndUsec+=30000000; bench.Record(sample);
    Require(bench.NeedsLoad(),"hard cap advances incomplete case");
    bench.Loaded(sample.frame.wallEndUsec);
    sample.frame.wallEndUsec-=1; bench.Record(sample);
    Require(!bench.Active(),"clock regression aborts");
    Require(VitaBenchmark_SourceSeed()==0,"abort restores source seed");
    for (uint32_t round : {2u,3u,4u,5u,6u,7u,8u}) {
    Require(bench.Start(round==2?"benchmark-shading.jsonl":round==3?"benchmark-material.jsonl":"benchmark-normal.jsonl","test","abc",round),"start shader round");
    const unsigned segmentCount=round==7?7u:5u;
    Require(bench.CaseCount()==segmentCount,"requested segment count");
    clock=1;
    for (unsigned c=0;c<segmentCount;++c) {
        const uint32_t mode=round==8?(c==3?7:0):round>=5?0:c==1?(round==2?1:round==3?3:5):c==3?(round==2?2:round==3?4:6):0;
        Require(bench.Case().shaderMode==mode,"one factor and restored baselines");
        Require(bench.Case().width==960 && bench.Case().height==544,"native scene stays fixed");
        bench.Loaded(clock);
        VitaBenchmarkFrame frame{};
        frame.audioDetail.mode=round==6?(c==1?1:c==3?2:0):0;
        Require(bench.Case().audioMode==frame.audioDetail.mode,"audio is the only changed factor");
        frame.geometry.mode=round==8?(c==1?3:0):round==5?(c==1?1:c==3?2:0):0;
        Require(bench.Case().geometryMode==frame.geometry.mode,"geometry mode restores with each baseline");
        frame.textures.mode=round==7?(c==1?1:c==3?2:c==5?3:0):0;
        Require(bench.Case().textureMode==frame.textures.mode,"texture scope restored per case");
        frame.shaderMode=mode; frame.sceneShaderDraws=300;
        frame.simpleMaterialDraws=mode==2?300:0;
        frame.noLocalLightDraws=mode==3?300:0; frame.noSpecularDraws=mode==4?300:0;
        frame.specularEligibleDraws=20; frame.nonSunLightInputs=1;
        frame.noNormalMapDraws=mode==6?300:0; frame.normalEligibleDraws=30;
        frame.adaptiveFullLightDraws=mode==5?300:0;
        frame.postCopyDraws=mode==1?1:0; frame.authoredPostDraws=mode==1?0:1;
        clock+=10000000; frame.frame.wallEndUsec=clock; bench.Record(frame);
        for (unsigned n=0;n<60;++n) {
            TestInput input{};
            input.buttons=1; input.forwardmove=127; input.lookX=72;
            input.lookDegrees[0]=4.0f; input.viewAngles[1]=12000;
            input.rearFragHeld=true; input.rearSpecialHeld=true;
            input.rearSprintHeld=true; input.rearMeleePressed=true;
            input.weaponCyclePressed=true;
            bench.FilterInput(input,32,2);
            Require(bench.Active(),"incidental input does not abort");
            Require(!input.buttons && !input.forwardmove && !input.lookX
                && !input.lookDegrees[0] && !input.viewAngles[1]
                && !input.rearFragHeld && !input.rearSpecialHeld
                && !input.rearSprintHeld && !input.rearMeleePressed
                && !input.weaponCyclePressed,"all gameplay channels blocked before source publication");
            clock+=1000000; frame.frame.wallEndUsec=clock; bench.Record(frame);
        }
        Require(c+1==segmentCount?!bench.Active():bench.NeedsLoad(),"shading advances and ends automatically");
    }
    Require(VitaBenchmark_SourceSeed()==0,"shading completion restores seed");
    }
    Require(!bench.Start("invalid-round.jsonl","test","abc",10),"unknown round rejected");
    std::ifstream blockedFile("benchmark-shading.jsonl"); std::stringstream blocked;
    blocked<<blockedFile.rdbuf();
    Require(blocked.str().find("\"blockedInputFrames\":300")!=std::string::npos,"blocked frame total persists");
    Require(blocked.str().find("\"blockedInputRecords\":32")!=std::string::npos,"input detail log is bounded");
    for (int cancel=0;cancel<2;++cancel) {
        Require(bench.Start(cancel?"benchmark-circle.jsonl":"benchmark-start.jsonl","test","abc",2),"cancel test start");
        TestInput input{}; input.startPressed=!cancel; input.menuBackPressed=cancel;
        bench.FilterInput(input);
        Require(!bench.Active() && VitaBenchmark_SourceSeed()==0,"Start/Circle still cancel");
    }
    TestInput gameplay{}; gameplay.buttons=17; gameplay.forwardmove=127;
    bench.FilterInput(gameplay);
    Require(gameplay.buttons==17 && gameplay.forwardmove==127,"ordinary play inputs unchanged");
    Require(bench.Start("benchmark-balanced.jsonl","test","abc",9),"optional combined round starts");
    Require(bench.CaseCount()==3,"one candidate bracketed by baselines");
    for(unsigned c=0;c<3;++c) {
        Require(bench.Case().width==(c==1?720:960),"combined world dimensions");
        Require(bench.Case().textureMode==0 && bench.Case().audioMode==0,"combined quality preserved");
        bench.Loaded(clock);
        VitaBenchmarkFrame frame{}; clock+=10000000; frame.frame.wallEndUsec=clock; bench.Record(frame);
        for(unsigned n=0;n<60;++n) {clock+=1000000;frame.frame.wallEndUsec=clock;bench.Record(frame);}
        Require(c==2?!bench.Active():bench.NeedsLoad(),"combined round completes and restores source seed");
    }
    Require(VitaBenchmark_SourceSeed()==0,"combined completion restores source RNG");
    std::puts("benchmark tests passed");
}
