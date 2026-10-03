#include <vita/vita_script_runtime.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <vita/vita_script_vm_runtime.h>

namespace kisak::vita {
namespace {

std::string StripGscCommentsAndStrings(const std::string& source) {
    std::string clean(source.size(), ' ');
    enum class State { Code, LineComment, BlockComment, String };
    State state = State::Code;
    for (size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        switch (state) {
        case State::Code:
            if (c == '/' && next == '/') {
                state = State::LineComment;
                ++i;
            } else if (c == '/' && next == '*') {
                state = State::BlockComment;
                ++i;
            } else if (c == '"') {
                state = State::String;
            } else {
                clean[i] = c;
            }
            break;
        case State::LineComment:
            if (c == '\n' || c == '\r') {
                clean[i] = c;
                state = State::Code;
            }
            break;
        case State::BlockComment:
            if (c == '*' && next == '/') {
                ++i;
                state = State::Code;
            }
            break;
        case State::String:
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                state = State::Code;
            }
            break;
        }
    }
    return clean;
}

// Keep quoted tokens while removing comments. The source gametype registers
// several score events; unlike the dvar helpers, the first numeric argument
// is not enough to identify the authored value because another event may be
// registered before "kill". This parallel view preserves the event name at
// the same offsets as the code-only view below.
std::string StripGscCommentsPreserveStrings(const std::string& source) {
    std::string clean(source.size(), ' ');
    enum class State { Code, LineComment, BlockComment, String };
    State state = State::Code;
    for (size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        switch (state) {
        case State::Code:
            if (c == '/' && next == '/') {
                state = State::LineComment;
                ++i;
            } else if (c == '/' && next == '*') {
                state = State::BlockComment;
                ++i;
            } else {
                clean[i] = c;
                if (c == '"')
                    state = State::String;
            }
            break;
        case State::LineComment:
            if (c == '\n' || c == '\r') {
                clean[i] = c;
                state = State::Code;
            }
            break;
        case State::BlockComment:
            if (c == '*' && next == '/') {
                ++i;
                state = State::Code;
            } else if (c == '\n' || c == '\r') {
                clean[i] = c;
            }
            break;
        case State::String:
            clean[i] = c;
            if (c == '\\' && i + 1 < source.size()) {
                const size_t escaped = i + 1u;
                clean[escaped] = source[escaped];
                i = escaped;
            } else if (c == '"') {
                state = State::Code;
            }
            break;
        }
    }
    return clean;
}

bool HasFunctionDefinition(const std::string& source, const char* function) {
    if (!function || !*function)
        return false;
    const size_t functionLength = std::strlen(function);
    size_t cursor = 0;
    while ((cursor = source.find(function, cursor)) != std::string::npos) {
        const bool leftBoundary = cursor == 0
            || !(std::isalnum(static_cast<unsigned char>(source[cursor - 1]))
                 || source[cursor - 1] == '_');
        size_t after = cursor + functionLength;
        while (after < source.size()
               && std::isspace(static_cast<unsigned char>(source[after])))
            ++after;
        const bool call = after < source.size() && source[after] == '(';
        if (leftBoundary && call) {
            int depth = 0;
            size_t close = after;
            for (; close < source.size(); ++close) {
                if (source[close] == '(')
                    ++depth;
                else if (source[close] == ')' && --depth == 0)
                    break;
            }
            if (depth == 0) {
                while (++close < source.size()
                       && std::isspace(static_cast<unsigned char>(source[close]))) {}
                if (close < source.size() && source[close] == '{')
                    return true;
            }
        }
        cursor += functionLength;
    }
    return false;
}

bool ProgramReady(const std::vector<VitaScriptProgramInfo>& programs,
                  const char* name, bool main, bool init, bool connect,
                  bool spawn, bool damage, bool killed) {
    const auto found = std::find_if(programs.begin(), programs.end(),
        [name](const VitaScriptProgramInfo& program) {
            return program.name == name;
        });
    if (found == programs.end())
        return false;
    return (!main || found->hasMain) && (!init || found->hasInit)
        && (!connect || found->hasPlayerConnect)
        && (!spawn || found->hasPlayerSpawn)
        && (!damage || found->hasPlayerDamage)
        && (!killed || found->hasPlayerKilled);
}

bool ParseIntegerAtOrAfter(const std::string& source, size_t start,
                           int32_t* output) {
    if (!output)
        return false;
    for (size_t cursor = std::min(start, source.size()); cursor < source.size();
         ++cursor) {
        const char character = source[cursor];
        const bool signedDigit = (character == '-' || character == '+')
            && cursor + 1u < source.size()
            && std::isdigit(static_cast<unsigned char>(source[cursor + 1u]));
        if (!std::isdigit(static_cast<unsigned char>(character)) && !signedDigit)
            continue;
        char* end = nullptr;
        const long parsed = std::strtol(source.c_str() + cursor, &end, 10);
        if (end == source.c_str() + cursor
            || parsed < static_cast<long>(INT32_MIN)
            || parsed > static_cast<long>(INT32_MAX))
            return false;
        *output = static_cast<int32_t>(parsed);
        return true;
    }
    return false;
}

bool ParseFirstIntegerArgument(const std::string& source, const char* function,
                               int32_t* output) {
    if (!function || !*function || !output)
        return false;
    const size_t functionBytes = std::strlen(function);
    size_t cursor = 0;
    while ((cursor = source.find(function, cursor)) != std::string::npos) {
        const bool leftBoundary = cursor == 0
            || !(std::isalnum(static_cast<unsigned char>(source[cursor - 1u]))
                 || source[cursor - 1u] == '_');
        size_t open = cursor + functionBytes;
        while (open < source.size()
               && std::isspace(static_cast<unsigned char>(source[open])))
            ++open;
        if (!leftBoundary || open >= source.size() || source[open] != '(') {
            cursor += functionBytes;
            continue;
        }
        // The first numeric argument is the authored default for the dvar
        // registrations. Named score events use ParseNamedScoreInfo below so
        // a headshot/assist registration cannot be mistaken for the kill
        // value when source order changes.
        int depth = 1;
        for (size_t argument = open + 1u; argument < source.size(); ++argument) {
            const char character = source[argument];
            if (character == '(') {
                ++depth;
                continue;
            }
            if (character == ')') {
                if (--depth == 0)
                    break;
                continue;
            }
            if (depth == 1) {
                const bool signedDigit = (character == '-' || character == '+')
                    && argument + 1u < source.size()
                    && std::isdigit(static_cast<unsigned char>(source[argument + 1u]));
                if (std::isdigit(static_cast<unsigned char>(character)) || signedDigit)
                    return ParseIntegerAtOrAfter(source, argument, output);
            }
        }
        cursor = open + 1u;
    }
    return false;
}

bool ParseNamedScoreInfo(const std::string& source, const char* eventName,
                         int32_t* output) {
    if (!eventName || !*eventName || !output)
        return false;
    constexpr char kFunction[] = "registerScoreInfo";
    constexpr size_t kFunctionBytes = sizeof(kFunction) - 1u;
    const std::string code = StripGscCommentsAndStrings(source);
    const std::string strings = StripGscCommentsPreserveStrings(source);
    size_t cursor = 0;
    while ((cursor = code.find(kFunction, cursor)) != std::string::npos) {
        const bool leftBoundary = cursor == 0
            || !(std::isalnum(static_cast<unsigned char>(code[cursor - 1u]))
                 || code[cursor - 1u] == '_');
        size_t open = cursor + kFunctionBytes;
        while (open < code.size() && std::isspace(
                   static_cast<unsigned char>(code[open])))
            ++open;
        if (!leftBoundary || open >= code.size() || code[open] != '(') {
            cursor += kFunctionBytes;
            continue;
        }
        int depth = 1;
        size_t close = open + 1u;
        for (; close < code.size(); ++close) {
            if (code[close] == '(')
                ++depth;
            else if (code[close] == ')' && --depth == 0)
                break;
        }
        if (depth != 0)
            return false;
        size_t argument = open + 1u;
        // The code-only view replaces every character inside a quoted token
        // with spaces.  Stop at the opening quote in the parallel preserved
        // view instead of walking through the whole event name to the comma.
        while (argument < close
               && std::isspace(static_cast<unsigned char>(code[argument]))
               && strings[argument] != '"')
            ++argument;
        if (argument >= close || strings[argument] != '"') {
            cursor = close + 1u;
            continue;
        }
        std::string parsedName;
        size_t nameEnd = argument + 1u;
        for (; nameEnd < close; ++nameEnd) {
            const char character = strings[nameEnd];
            if (character == '\\' && nameEnd + 1u < close) {
                parsedName.push_back(strings[++nameEnd]);
            } else if (character == '"') {
                break;
            } else {
                parsedName.push_back(character);
            }
        }
        if (nameEnd >= close || parsedName != eventName) {
            cursor = close + 1u;
            continue;
        }
        // Restrict the value scan to this call so malformed source cannot
        // borrow a number from a later registration.
        for (size_t value = nameEnd + 1u; value < close; ++value) {
            const char character = code[value];
            const bool signedDigit = (character == '-' || character == '+')
                && value + 1u < close
                && std::isdigit(static_cast<unsigned char>(code[value + 1u]));
            if (!std::isdigit(static_cast<unsigned char>(character))
                && !signedDigit)
                continue;
            char* end = nullptr;
            const long parsed = std::strtol(code.c_str() + value, &end, 10);
            if (end == code.c_str() + value
                || parsed < static_cast<long>(INT32_MIN)
                || parsed > static_cast<long>(INT32_MAX))
                return false;
            *output = static_cast<int32_t>(parsed);
            return true;
        }
        return false;
    }
    return false;
}

bool ParseRetailDeathmatchRules(const std::string& source,
                                ShipmentDmRules* output) {
    if (!output)
        return false;
    const std::string clean = StripGscCommentsAndStrings(source);
    int32_t timeLimitMinutes = 0;
    int32_t scoreLimit = 0;
    int32_t killScore = 0;
    if (!ParseFirstIntegerArgument(clean, "registerTimeLimitDvar",
                                   &timeLimitMinutes)
        || !ParseFirstIntegerArgument(clean, "registerScoreLimitDvar",
                                      &scoreLimit)
        || !ParseNamedScoreInfo(source, "kill", &killScore))
        return false;
    if (timeLimitMinutes < 0 || timeLimitMinutes > 1440
        || scoreLimit < 0 || scoreLimit > 5000
        || killScore < 0 || killScore > 1000)
        return false;
    const int64_t timeLimitMsec = static_cast<int64_t>(timeLimitMinutes)
        * 60ll * 1000ll;
    if (timeLimitMsec > static_cast<int64_t>(INT32_MAX))
        return false;
    ShipmentDmRules parsed{};
    parsed.timeLimitMsec = static_cast<int32_t>(timeLimitMsec);
    parsed.scoreLimit = scoreLimit;
    parsed.killScore = killScore;
    // dm.gsc is the free-for-all ruleset.  If a future source script begins
    // assigning level.teamBased explicitly, honor a numeric/boolean value
    // without changing today's authored false default.
    const size_t teamField = clean.find("level.teamBased");
    if (teamField != std::string::npos) {
        const size_t assignment = clean.find('=', teamField + 1u);
        if (assignment != std::string::npos) {
            size_t value = assignment + 1u;
            while (value < clean.size()
                   && std::isspace(static_cast<unsigned char>(clean[value])))
                ++value;
            if (clean.compare(value, 4u, "true") == 0)
                parsed.teamBased = true;
            else if (clean.compare(value, 5u, "false") == 0)
                parsed.teamBased = false;
            else {
                int32_t numeric = 0;
                if (ParseIntegerAtOrAfter(clean, value, &numeric))
                    parsed.teamBased = numeric != 0;
            }
        }
    }
    *output = parsed;
    return true;
}

bool ParseRetailSetExpFog(const std::string& source, VitaScriptFogParams* output) {
    if (!output)
        return false;
    const std::string clean = StripGscCommentsAndStrings(source);
    constexpr char kFunction[] = "setExpFog";
    constexpr size_t kFunctionBytes = sizeof(kFunction) - 1u;
    size_t call = std::string::npos;
    for (size_t cursor = 0; cursor + kFunctionBytes <= clean.size(); ++cursor) {
        bool matches = true;
        for (size_t character = 0; character < kFunctionBytes; ++character) {
            if (std::tolower(static_cast<unsigned char>(clean[cursor + character]))
                != std::tolower(static_cast<unsigned char>(kFunction[character]))) {
                matches = false;
                break;
            }
        }
        if (!matches)
            continue;
        const bool leftBoundary = cursor == 0
            || !(std::isalnum(static_cast<unsigned char>(clean[cursor - 1]))
                 || clean[cursor - 1] == '_');
        const size_t afterName = cursor + kFunctionBytes;
        if (!leftBoundary || afterName >= clean.size())
            continue;
        size_t open = afterName;
        while (open < clean.size()
               && std::isspace(static_cast<unsigned char>(clean[open])))
            ++open;
        if (open < clean.size() && clean[open] == '(') {
            call = open + 1u;
            break;
        }
    }
    if (call == std::string::npos)
        return false;

    float values[6]{};
    const char* cursor = clean.c_str() + call;
    char* end = nullptr;
    for (size_t index = 0; index < std::size(values); ++index) {
        while (*cursor && std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
        values[index] = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(values[index]))
            return false;
        cursor = end;
        while (*cursor && std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
        if (index + 1u < std::size(values)) {
            if (*cursor != ',')
                return false;
            ++cursor;
        } else if (*cursor != ')') {
            return false;
        }
    }
    if (values[0] < 0.0f || values[1] <= 0.0f
        || values[2] < 0.0f || values[2] > 1.0f
        || values[3] < 0.0f || values[3] > 1.0f
        || values[4] < 0.0f || values[4] > 1.0f
        || values[5] < 0.0f)
        return false;
    output->enabled = true;
    output->startDistance = values[0];
    output->halfwayDistance = values[1];
    output->density = 0.69314718f / values[1];
    output->color[0] = values[2];
    output->color[1] = values[3];
    output->color[2] = values[4];
    output->transitionSeconds = values[5];
    return output->density > 0.0f && std::isfinite(output->density);
}

} // namespace

void VitaScriptRuntime::Clear() {
    programs_.clear();
    loaded_ = false;
    shipmentClosureReady_ = false;
    matchStarted_ = false;
    notificationCount_ = 0;
    callbackQueue_.fill(VitaScriptCallbackEvent{});
    callbackHead_ = 0;
    callbackCount_ = 0;
    droppedCallbackCount_ = 0;
    fog_ = VitaScriptFogParams{};
    dmRules_ = ShipmentDmRules{};
    dmRulesLoaded_ = false;
}

VitaScriptProgramInfo VitaScriptRuntime::Inspect(const char* name,
                                                 const uint8_t* bytes,
                                                 size_t size) {
    VitaScriptProgramInfo info{};
    if (!name || !bytes || size == 0)
        return info;
    info.name = name;
    const std::string source = StripGscCommentsAndStrings(
        std::string(reinterpret_cast<const char*>(bytes), size));
    const auto has = [&source](const char* function) {
        return HasFunctionDefinition(source, function);
    };
    // Count top-level-looking definitions conservatively. This is not a VM;
    // it is an indexed source boundary used to select real callback targets.
    for (size_t cursor = 0; cursor < source.size();) {
        while (cursor < source.size()
               && !(std::isalpha(static_cast<unsigned char>(source[cursor]))
                    || source[cursor] == '_'))
            ++cursor;
        const size_t begin = cursor;
        while (cursor < source.size()
               && (std::isalnum(static_cast<unsigned char>(source[cursor]))
                   || source[cursor] == '_'))
            ++cursor;
        if (begin == cursor)
            continue;
        size_t after = cursor;
        while (after < source.size()
               && std::isspace(static_cast<unsigned char>(source[after])))
            ++after;
        if (after < source.size() && source[after] == '(') {
            int depth = 0;
            size_t close = after;
            for (; close < source.size(); ++close) {
                if (source[close] == '(')
                    ++depth;
                else if (source[close] == ')' && --depth == 0)
                    break;
            }
            while (depth == 0 && ++close < source.size()
                   && std::isspace(static_cast<unsigned char>(source[close]))) {}
            if (depth == 0 && close < source.size() && source[close] == '{')
                ++info.functionCount;
        }
    }
    info.hasMain = has("main");
    info.hasInit = has("init");
    info.hasPlayerConnect = has("onPlayerConnect")
        || has("CodeCallback_PlayerConnect");
    info.hasPlayerSpawn = has("onSpawnPlayer");
    info.hasPlayerDamage = has("CodeCallback_PlayerDamage");
    info.hasPlayerKilled = has("CodeCallback_PlayerKilled")
        || has("onPlayerKilled");
    return info;
}

bool VitaScriptRuntime::Load(const VitaRawFileBundle& rawfiles) {
    Clear();
    if (!rawfiles.Loaded())
        return false;
    programs_.reserve(rawfiles.Count());
    for (uint32_t index = 0; index < rawfiles.Count(); ++index) {
        const VitaRawFileRecord* record = rawfiles.Record(index);
        if (!record || record->bytes.empty())
            continue;
        // Only GSC sources participate in the callback closure. The bundle
        // also contains vision/shock/rumble/config records which are retained
        // for their own runtime consumers.
        const size_t extension = record->name.find_last_of('.');
        if (extension == std::string::npos
            || record->name.substr(extension) != ".gsc")
            continue;
        programs_.push_back(Inspect(record->name.c_str(), record->bytes.data(),
                                    record->bytes.size()));
        if (record->name == "maps/createart/mp_shipment_art.gsc") {
            ParseRetailSetExpFog(
                std::string(reinterpret_cast<const char*>(record->bytes.data()),
                            record->bytes.size()),
                &fog_);
        }
        if (record->name == "maps/mp/gametypes/dm.gsc") {
            dmRulesLoaded_ = ParseRetailDeathmatchRules(
                std::string(reinterpret_cast<const char*>(record->bytes.data()),
                            record->bytes.size()),
                &dmRules_);
        }
    }
    loaded_ = !programs_.empty();
    return loaded_;
}

bool VitaScriptRuntime::ParseDeathmatchRules(const char* source, size_t size,
                                             ShipmentDmRules* output) {
    if (!source || size == 0 || !output)
        return false;
    return ParseRetailDeathmatchRules(std::string(source, size), output);
}

bool VitaScriptRuntime::StartShipmentMatch() {
    if (!loaded_)
        return false;
    // These are the actual retail entry points reached by mp_shipment.gsc.
    // The callback script supplies the code-to-GSC hooks used by DM.
    shipmentClosureReady_
        = ProgramReady(programs_, "maps/mp/mp_shipment.gsc", true, false,
                       false, false, false, false)
        && ProgramReady(programs_, "maps/mp/mp_shipment_fx.gsc", true, false,
                        false, false, false, false)
        && ProgramReady(programs_, "maps/createart/mp_shipment_art.gsc", true,
                        false, false, false, false, false)
        && ProgramReady(programs_, "maps/mp/_load.gsc", true, false, false,
                        false, false, false)
        && ProgramReady(programs_, "maps/mp/gametypes/dm.gsc", true, false,
                        false, true, false, false)
        && ProgramReady(programs_, "maps/mp/gametypes/_spawnlogic.gsc", false,
                        true, true, false, false, false)
        && ProgramReady(programs_, "maps/mp/gametypes/_callbacksetup.gsc", false,
                        false, true, false, true, true);
    // A Shipment match is not considered runnable until the source dm.gsc
    // registration calls were recovered.  This prevents the Vita path from
    // silently falling back to a platform-only timer/score preset when an
    // incomplete rawfile bundle is mounted.
    shipmentClosureReady_ = shipmentClosureReady_ && dmRulesLoaded_;
    matchStarted_ = shipmentClosureReady_;
    lastCallback_.clear();
    return matchStarted_;
}

void VitaScriptRuntime::NotifyShipmentEvent(uint8_t eventType, uint32_t clientNum,
                                            int32_t serverTime,
                                            uint32_t attackerClientNum,
                                            int32_t damage, uint32_t weapon,
                                            const float* point,
                                            const float* direction) {
    if (!matchStarted_)
        return;
    // ShipmentDmEventType values are deliberately stable in the local slice:
    // Spawn=0, Fire=1, Reload=2, Respawn=3, Damage=4, Death=5, Kill=6.
    // Resolve only callbacks that exist in the retail closure. Fire/reload
    // and score bookkeeping have no corresponding GSC callback here.
    const char* callback = nullptr;
    switch (eventType) {
    case 0: // spawn
    case 3: // respawn
        callback = "maps/mp/gametypes/dm.gsc::onSpawnPlayer";
        break;
    case 4: // damage
        callback = "maps/mp/gametypes/_callbacksetup.gsc::CodeCallback_PlayerDamage";
        break;
    case 5: // killed
        callback = "maps/mp/gametypes/_callbacksetup.gsc::CodeCallback_PlayerKilled";
        break;
    default:
        break;
    }
    if (!callback)
        return;
    // The source index selects the semantic callback name, while the native
    // VM must have resolved the corresponding function handle during boot.
    // This keeps the bounded event transport from reporting a callback that
    // the real compiled Shipment closure cannot execute.
    if (!VitaScriptVm_HasCallback(eventType))
        return;
    lastCallback_ = callback;
    ++notificationCount_;
    if (callbackCount_ >= kCallbackQueueCapacity) {
        ++droppedCallbackCount_;
        return;
    }
    const size_t writeIndex = (callbackHead_ + callbackCount_)
        % kCallbackQueueCapacity;
    VitaScriptCallbackEvent& queued = callbackQueue_[writeIndex];
    queued = VitaScriptCallbackEvent{};
    queued.eventType = eventType;
    queued.clientNum = clientNum;
    queued.serverTime = serverTime;
    queued.functionHandle = VitaScriptVm_CallbackHandle(eventType);
    queued.attackerClientNum = attackerClientNum;
    queued.damage = damage;
    queued.weapon = weapon;
    if (point)
        std::memcpy(queued.point, point, sizeof(queued.point));
    if (direction)
        std::memcpy(queued.direction, direction, sizeof(queued.direction));
    ++callbackCount_;
}

bool VitaScriptRuntime::PollCallbackEvent(VitaScriptCallbackEvent* event) {
    if (!event || callbackCount_ == 0)
        return false;
    *event = callbackQueue_[callbackHead_];
    callbackQueue_[callbackHead_] = VitaScriptCallbackEvent{};
    callbackHead_ = (callbackHead_ + 1) % kCallbackQueueCapacity;
    --callbackCount_;
    return true;
}

const VitaScriptProgramInfo* VitaScriptRuntime::Find(const char* name) const {
    if (!name)
        return nullptr;
    const auto found = std::find_if(programs_.begin(), programs_.end(),
        [name](const VitaScriptProgramInfo& program) {
            return program.name == name;
        });
    return found == programs_.end() ? nullptr : &*found;
}

} // namespace kisak::vita
