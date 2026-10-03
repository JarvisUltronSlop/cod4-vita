#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vita/vita_rawfile_bundle.h>
#include <vita/vita_dm_rules.h>

namespace kisak::vita {

// This is the bounded source-index boundary used by the Shipment slice.  The
// native VM owns compilation/execution; this class verifies the real GSC entry
// points that compose the Shipment/deathmatch bootstrap and queues
// handle-gated callback events for the Vita entity adapter.
struct VitaScriptProgramInfo {
    std::string name;
    uint32_t functionCount = 0;
    bool hasMain = false;
    bool hasInit = false;
    bool hasPlayerConnect = false;
    bool hasPlayerSpawn = false;
    bool hasPlayerDamage = false;
    bool hasPlayerKilled = false;
};

// A callback event is the bounded handoff between the Vita local-DM
// transport and the native GSC/entity adapter. It is queued only after the
// native VM resolved its function handle, preserving the exact event payload
// without invoking an uninitialized entity context.
struct VitaScriptCallbackEvent {
    uint8_t eventType = 0;
    uint32_t clientNum = 0;
    int32_t serverTime = 0;
    uint32_t functionHandle = 0;
    uint32_t attackerClientNum = UINT32_MAX;
    int32_t damage = 0;
    uint32_t weapon = 0;
    float point[3] = {0.0f, 0.0f, 0.0f};
    float direction[3] = {0.0f, 0.0f, 1.0f};
};

// Pointer-free copy of the retail setExpFog state emitted by the Shipment
// createart script.  The Vita renderer consumes the same source values after
// the script boundary; no platform-specific fog color or density is invented
// here.
struct VitaScriptFogParams {
    bool enabled = false;
    float startDistance = 0.0f;
    float halfwayDistance = 0.0f;
    float density = 0.0f;
    float color[3] = {0.0f, 0.0f, 0.0f};
    float transitionSeconds = 0.0f;
};

class VitaScriptRuntime {
public:
    void Clear();
    bool Load(const VitaRawFileBundle& rawfiles);

    // Parse one source-authored dm.gsc ruleset without mounting a package.
    // Load() uses the same helper; this bounded entry point lets the ARM
    // smoke target prove that score registration is selected by event name,
    // not by whichever score event happens to appear first in the script.
    static bool ParseDeathmatchRules(const char* source, size_t size,
                                     ShipmentDmRules* output);

    // Validates the actual retail Shipment map and MP deathmatch callback
    // closure. StartShipmentMatch remains separate from Load so the source
    // index can be checked before the native VM/entity lifecycle starts.
    bool StartShipmentMatch();
    bool Loaded() const { return loaded_; }
    bool MatchStarted() const { return matchStarted_; }
    bool ShipmentClosureReady() const { return shipmentClosureReady_; }
    uint32_t ProgramCount() const { return static_cast<uint32_t>(programs_.size()); }
    uint32_t NotificationCount() const { return notificationCount_; }
    bool PollCallbackEvent(VitaScriptCallbackEvent* event);
    uint32_t PendingCallbackCount() const {
        return static_cast<uint32_t>(callbackCount_);
    }
    uint32_t DroppedCallbackCount() const { return droppedCallbackCount_; }
    const char* LastCallback() const { return lastCallback_.c_str(); }
    const VitaScriptFogParams& Fog() const { return fog_; }
    // The authored dm.gsc registers these values at gametype startup.  Keep
    // the parsed copy available to the server initializer so the local match
    // does not silently diverge from the source script.
    bool DeathmatchRulesLoaded() const { return dmRulesLoaded_; }
    const ShipmentDmRules& DeathmatchRules() const { return dmRules_; }

    // Map server events onto the callback names present in the real GSC
    // closure. Unsupported events are ignored rather than inventing script
    // behavior that is not represented by the retail source.
    void NotifyShipmentEvent(uint8_t eventType, uint32_t clientNum,
                             int32_t serverTime, uint32_t attackerClientNum = UINT32_MAX,
                             int32_t damage = 0, uint32_t weapon = 0,
                             const float* point = nullptr,
                             const float* direction = nullptr);

    const VitaScriptProgramInfo* Find(const char* name) const;

private:
    static VitaScriptProgramInfo Inspect(const char* name, const uint8_t* bytes,
                                         size_t size);

    std::vector<VitaScriptProgramInfo> programs_;
    bool loaded_ = false;
    bool shipmentClosureReady_ = false;
    bool matchStarted_ = false;
    uint32_t notificationCount_ = 0;
    static constexpr size_t kCallbackQueueCapacity = 64;
    std::array<VitaScriptCallbackEvent, kCallbackQueueCapacity> callbackQueue_{};
    size_t callbackHead_ = 0;
    size_t callbackCount_ = 0;
    uint32_t droppedCallbackCount_ = 0;
    std::string lastCallback_;
    VitaScriptFogParams fog_{};
    ShipmentDmRules dmRules_{};
    bool dmRulesLoaded_ = false;
};

} // namespace kisak::vita
