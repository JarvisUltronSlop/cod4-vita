#pragma once
#include <cstdint>
#include <cstdio>

namespace kisak::vita {
// Wall-time controller for a fully rendered Shipment run. It supplies physical
// input equivalents only; it never changes simulation time, state or RNG.
class VitaShipmentAutoTest {
public:
    static constexpr uint64_t kDurationUs=180000000;
    enum class State { Idle, Loading, Running, Complete, Cancelled, Failed };
    enum class Event { None, Began, Completed, Cancelled, Failed };
    struct Controls {
        int8_t forward=0,right=0;
        float yawDegrees=0;
        bool fire=false,ads=false,reload=false,crouch=false,jump=false;
        bool cycle=false,fragHeld=false,fragPressed=false;
    };
    bool Request(bool mainMenu,bool requested,uint64_t now) {
        if(!mainMenu || !requested || Active() || run_==UINT32_MAX)return false;
        ++run_;state_=State::Loading;requestedAt_=lastNow_=now;startedAt_=elapsed_=0;
        phase_=UINT32_MAX;skipped_=0;lastYaw_=0;interrupted_=logFailure_=false;
        phaseChanged_=false;controls_={};nextReport_=now;return true;
    }
    Event Tick(uint64_t now,bool playable,bool fatal,bool cancel,bool interrupted) {
        controls_={};phaseChanged_=false;
        if(!Active())return Event::None;
        if(fatal || now<lastNow_) {state_=State::Failed;return Event::Failed;}
        lastNow_=now;
        if(state_==State::Running)elapsed_=now-startedAt_;
        // An interruption wins over a deadline crossed while the app slept.
        if(cancel || interrupted || interrupted_) {state_=State::Cancelled;return Event::Cancelled;}
        Event event=Event::None;
        if(state_==State::Loading) {
            if(!playable)return event;
            state_=State::Running;startedAt_=now;elapsed_=0;event=Event::Began;
        } else if(!playable) {state_=State::Failed;return Event::Failed;}
        if(elapsed_>=kDurationUs) {
            if(phase_!=UINT32_MAX && phase_<59)skipped_+=59-phase_;
            state_=State::Complete;return Event::Completed;
        }
        const uint32_t ms=uint32_t(elapsed_/1000), within=ms%30000;
        constexpr uint32_t ends[]={4000,8000,12000,14000,18000,22000,24000,26000,28000,30000};
        uint32_t step=0;while(within>=ends[step])++step;
        const uint32_t nextPhase=(ms/30000)*10+step;
        phaseChanged_=nextPhase!=phase_;
        if(phase_!=UINT32_MAX && nextPhase>phase_+1)skipped_+=nextPhase-phase_-1;
        phase_=nextPhase;
        switch(step) {
        case 0:controls_.forward=64;break;
        case 1:controls_.right=64;controls_.fire=true;break;
        case 2:controls_.fire=controls_.ads=true;break;
        case 3:controls_.reload=true;break;
        case 4:controls_.forward=-48;controls_.crouch=true;break;
        case 5:controls_.right=-64;break;
        case 6:controls_.forward=48;controls_.jump=true;break;
        case 7:controls_.cycle=phaseChanged_;break;
        case 8:controls_.fragHeld=true;controls_.fragPressed=phaseChanged_;break;
        case 9:controls_.fire=true;break;
        }
        // Integrate angular velocity across phase boundaries. Look distance is
        // independent of polling/presentation rate, including delayed updates.
        const auto bounded=[](uint32_t value,uint32_t limit){return value<limit?value:limit;};
        const float yaw=0.008f*float(bounded(within,4000))
            -0.008f*float(within>18000?bounded(within-18000,4000):0);
        controls_.yawDegrees=yaw-lastYaw_;lastYaw_=yaw;
        return event;
    }
    void Interrupt() {if(Active())interrupted_=true;}
    bool ReportDue() {
        if(state_!=State::Running || lastNow_<nextReport_)return false;
        nextReport_=lastNow_+250000;return true;
    }
    void SetLogFailure(bool value) {logFailure_=value;}
    bool Active() const {return state_==State::Loading || state_==State::Running;}
    State Status() const {return state_;}
    uint32_t Run() const {return run_;}
    uint32_t Phase() const {return phase_;}
    bool PhaseChanged() const {return phaseChanged_;}
    uint32_t SkippedPhases() const {return skipped_;}
    uint64_t ElapsedUs() const {return elapsed_;}
    uint64_t LoadUs() const {return startedAt_>=requestedAt_?startedAt_-requestedAt_:0;}
    const Controls& Input() const {return controls_;}
    void Label(char* output,size_t bytes,bool mainMenu) const {
        if(!output || !bytes)return;
        output[0]=0;
        if(state_==State::Loading)std::snprintf(output,bytes,"3 MIN SHIPMENT TEST - LOADING - TIMER STARTS IN GAME");
        else if(state_==State::Running) {
            const uint32_t seconds=uint32_t((kDurationUs-elapsed_+999999)/1000000);
            std::snprintf(output,bytes,"SHIPMENT TEST %u:%02u LEFT - AUTOMATIC INPUT - START CANCELS",seconds/60,seconds%60);
        } else if(mainMenu && state_!=State::Idle) {
            std::snprintf(output,bytes,"TEST %s%s - COPY LOGS BEFORE RELAUNCH",
                state_==State::Complete?"COMPLETE":state_==State::Cancelled?"CANCELLED":"FAILED",
                logFailure_?" - LOG ERRORS":"");
        } else if(mainMenu)std::snprintf(output,bytes,"MAIN > TESTS: RUN 3 MIN SHIPMENT TEST");
    }
private:
    State state_=State::Idle;
    Controls controls_{};
    uint64_t requestedAt_=0,startedAt_=0,lastNow_=0,elapsed_=0,nextReport_=0;
    uint32_t run_=0,phase_=UINT32_MAX,skipped_=0;
    float lastYaw_=0;
    bool interrupted_=false,logFailure_=false,phaseChanged_=false;
};
}
