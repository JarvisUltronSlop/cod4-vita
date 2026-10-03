#pragma once

// Diagnostic controller stimulus only. Never compiled into the normal menu
// build, and never substitutes for a Windows/Vita usercmd parity comparison.
#if !defined(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
#error Source input smoke requires the explicit diagnostic autostart build
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vita/vita_input.h>

namespace kisak::vita {
class VitaSourceInputSmoke {
    struct Step {
        char label[32]{};
        uint32_t duration = 0, buttons = 0;
        int forward = 0, right = 0, cycle = 0;
        float pitch = 0, yaw = 0;
    };
    Step steps_[96]{};
    uint32_t count_ = 0, index_ = 0;
    int start_ = -1, previous_ = -1, phaseStart_ = -1;
    bool finished_ = false;

public:
    void Reset() { *this = {}; }
    bool Load() {
        Reset();
        FILE* file = std::fopen("ux0:data/CODVIT001/tests/source-input.csv", "rb");
        if (!file) return true; // Opt-in fixture; neutral smoke remains useful.
        char line[256]{};
        bool valid = std::fgets(line, sizeof(line), file)
            && (!std::strcmp(line, "COD4-VITA-INPUT-SMOKE 1\n")
                || !std::strcmp(line, "COD4-VITA-INPUT-SMOKE 1\r\n"));
        uint32_t total = 0;
        while (valid && std::fgets(line, sizeof(line), file)) {
            if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
            if (count_ == std::size(steps_)) { valid = false; break; }
            Step step{};
            char trailing = 0;
            const int fields = std::sscanf(line, "%31[a-zA-Z0-9_],%u,%x,%d,%d,%f,%f,%d %c",
                step.label, &step.duration, &step.buttons, &step.forward, &step.right,
                &step.pitch, &step.yaw, &step.cycle, &trailing);
            constexpr uint32_t allowed = BUTTON_ATTACK | BUTTON_SPRINT | BUTTON_MELEE
                | BUTTON_USE | BUTTON_RELOAD | BUTTON_USE_RELOAD | BUTTON_PRONE
                | BUTTON_CROUCH | BUTTON_JUMP | BUTTON_ADS | BUTTON_FRAG | BUTTON_SMOKE;
            valid = fields == 8 && step.duration > 0 && step.duration <= 10000
                && !(step.buttons & ~allowed) && step.forward >= -127 && step.forward <= 127
                && step.right >= -127 && step.right <= 127 && (step.cycle == 0 || step.cycle == 1)
                && std::isfinite(step.pitch) && std::fabs(step.pitch) <= 360
                && std::isfinite(step.yaw) && std::fabs(step.yaw) <= 360
                && total + step.duration <= 120000;
            if (valid) { steps_[count_++] = step; total += step.duration; }
        }
        valid = valid && !std::ferror(file) && count_ > 0;
        std::fclose(file);
        if (!valid) { Reset(); return false; }
        Com_Printf(0, "Vita source input smoke loaded steps=%u minimumMs=%u; diagnostic only\n", count_, total);
        return true;
    }

    void Apply(int serverTime, bool mayStart) {
        if (!count_ || (start_ < 0 && !mayStart)) return;
        VitaInputSample sample{};
        if (finished_) { VitaInput_PublishSourceSample(&sample); return; }
        bool entered = false;
        if (start_ < 0) {
            start_ = previous_ = phaseStart_ = serverTime;
            entered = true;
        }
        if (serverTime < previous_) {
            Com_Error(ERR_DROP, "Vita: source input smoke clock moved backwards");
            return;
        }
        // Admit each step at least once, including edge actions after a hitch.
        // Actual command times are recorded; this schedule is not parity proof.
        if (serverTime - phaseStart_ >= static_cast<int>(steps_[index_].duration)) {
            if (++index_ == count_) {
                finished_ = true;
                Com_Printf(0, "Vita source input smoke completed steps=%u elapsedMs=%d\n", count_, serverTime - start_);
                VitaInput_PublishSourceSample(&sample);
                return;
            }
            phaseStart_ = serverTime;
            entered = true;
        }
        const Step& step = steps_[index_];
        if (entered) Com_Printf(0, "Vita source input phase=%u name=%s time=%d\n", index_, step.label, serverTime);
        sample.buttons = step.buttons;
        if (!entered) sample.buttons &= ~BUTTON_MELEE;
        sample.forwardmove = static_cast<int8_t>(step.forward);
        sample.rightmove = static_cast<int8_t>(step.right);
        sample.lookDegrees[0] = step.pitch * (serverTime - previous_) * 0.001f;
        sample.lookDegrees[1] = step.yaw * (serverTime - previous_) * 0.001f;
        sample.weaponCyclePressed = entered && step.cycle;
        sample.rearFragHeld = (step.buttons & BUTTON_FRAG) != 0;
        sample.rearSpecialHeld = (step.buttons & BUTTON_SMOKE) != 0;
        sample.rearSprintHeld = (step.buttons & BUTTON_SPRINT) != 0;
        previous_ = serverTime;
        VitaInput_PublishSourceSample(&sample);
    }

    bool Recording() const { return count_ && start_ >= 0 && !finished_; }
    bool Completed() const { return count_ && finished_; }
    uint32_t Phase() const { return index_; }
};
} // namespace kisak::vita
