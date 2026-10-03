#pragma once

#include <cstdint>

namespace kisak::vita {

// Defaults recovered from the authored `maps/mp/gametypes/dm.gsc` in the
// read-only Steam install.  The parser replaces these at package load; the
// values remain available for standalone server diagnostics, while the
// production Shipment route requires the source registration to be present.
inline constexpr int32_t kShipmentDmTimeLimitMsec = 10 * 60 * 1000;
inline constexpr int32_t kShipmentDmScoreLimit = 1000;
inline constexpr int32_t kShipmentDmKillScore = 5;

// Typed rules shared by the source-script loader and the bounded local server.
// Keeping the values together prevents the HUD, scoreboard, and gameplay
// code from drifting when a source-authored gametype registration changes.
struct ShipmentDmRules {
    int32_t timeLimitMsec = kShipmentDmTimeLimitMsec;
    int32_t scoreLimit = kShipmentDmScoreLimit;
    int32_t killScore = kShipmentDmKillScore;
    bool teamBased = false;
};

} // namespace kisak::vita
