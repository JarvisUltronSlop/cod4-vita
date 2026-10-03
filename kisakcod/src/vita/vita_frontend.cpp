#include <vita/vita_diagnostic_glyph.h>
#include <vita/vita_frontend.h>
#include <vita/vita_map_assets.h>

#include <gfx_vita/gfx_vita_backend.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

enum class VitaFrontendAction : uint8_t {
    Disabled = 0,
    OpenMultiplayer,
    OpenCustomClasses,
    OpenOptions,
    OpenGraphics,
    OpenTests,
    RunShipmentTest,
    AdjustResolution,
    ToggleGrass,
    ToggleWorldLighting,
    RunComparison,
    RunCpuDiagnostic,
    RunGraphicsProfile,
    RunGpuTest,
    OpenControls,
    OpenControlsLook,
    OpenGameModeSelect,
    OpenMapSelect,
    Back,
    ResumeGame,
    ExitToMain,
    StartShipment,
    SelectDeathmatch,
    AdjustSensitivity,
    ToggleInvertY,
    ToggleRearTouch,
    ToggleDpadFallback,
    ResetControls,
};

struct VitaFrontendRouteItem {
    const char* localizationKey;
    const char* fallbackLabel;
    bool enabled;
    VitaFrontendAction action;
};

// The retail MP executable drives the boot route through script menus rather
// than exporting a single `main` menuDef. Keep the route as a small, explicit
// capability manifest: labels come from the real VITZ table, unsupported
// choices stay visible/disabled, and each enabled row has a named COD4 action.
constexpr VitaFrontendRouteItem kMainRoute[] = {
    {"MENU_SINGLE_PLAYER", "SINGLE PLAYER", false, VitaFrontendAction::Disabled},
    {"MENU_MULTIPLAYER", "MULTIPLAYER", true, VitaFrontendAction::OpenMultiplayer},
    {"MENU_OPTIONS", "OPTIONS", true, VitaFrontendAction::OpenOptions},
#if KISAK_VITA_CPU_PROFILE
    {"VITA_TESTS", "TESTS", true, VitaFrontendAction::OpenTests},
#endif
    {"MENU_QUIT", "QUIT", false, VitaFrontendAction::Disabled},
};

constexpr VitaFrontendRouteItem kMultiplayerRoute[] = {
    {"MENU_PLAYER_MATCH", "PLAYER MATCH", true, VitaFrontendAction::OpenGameModeSelect},
    {"MPUI_CREATE_A_CLASS_CAP", "CREATE A CLASS", false, VitaFrontendAction::Disabled},
    {"MPUI_BARRACKS_CAP", "BARRACKS", false, VitaFrontendAction::Disabled},
    {"MENU_CHALLENGES_CAP", "CHALLENGES", false, VitaFrontendAction::Disabled},
    {"MENU_MULTIPLAYER_OPTIONS", "OPTIONS", true, VitaFrontendAction::OpenOptions},
    {"MENU_BACK", "BACK", true, VitaFrontendAction::Back},
};

// This is the source `dm` entry point (localized by COD4 as Free-for-all).
// The remaining retail playlists stay present but disabled until their
// server rules, scoreboards, and objective HUD are ported.
constexpr VitaFrontendRouteItem kGameModeRoute[] = {
    {"MPUI_DEATHMATCH", "FREE-FOR-ALL", true, VitaFrontendAction::SelectDeathmatch},
    {"MPUI_TEAM_DEATHMATCH", "TEAM DEATHMATCH", false, VitaFrontendAction::Disabled},
    {"MPUI_DOMINATION", "DOMINATION", false, VitaFrontendAction::Disabled},
    {"MPUI_HEADQUARTERS", "HEADQUARTERS", false, VitaFrontendAction::Disabled},
    {"MPUI_SEARCH_AND_DESTROY", "SEARCH AND DESTROY", false, VitaFrontendAction::Disabled},
    {"MPUI_SABOTAGE", "SABOTAGE", false, VitaFrontendAction::Disabled},
    {"MENU_BACK", "BACK", true, VitaFrontendAction::Back},
};

constexpr VitaFrontendRouteItem kMapRoute[] = {
#define VITA_MAP(id, name, title, key, original) {key,title,original,VitaFrontendAction::StartShipment},
#include <vita/vita_map_catalog.inc>
#undef VITA_MAP
    {"MENU_BACK", "BACK", true, VitaFrontendAction::Back},
};

constexpr VitaFrontendRouteItem kOptionsRoute[] = {
    // Keep the retail PC options categories and geometry visible.  Controls
    // is the first capability that is genuinely wired on Vita; the remaining
    // desktop-only pages stay present but disabled rather than disappearing.
    {"MENU_CONTROLS", "CONTROLS", true, VitaFrontendAction::OpenControls},
    {"MENU_GRAPHICS", "GRAPHICS", true, VitaFrontendAction::OpenGraphics},
    {"MENU_TEXTURE_SETTINGS", "TEXTURE SETTINGS", false, VitaFrontendAction::Disabled},
    {"MENU_SOUND", "SOUND", false, VitaFrontendAction::Disabled},
    {"MENU_VOICECHAT", "VOICE CHAT", false, VitaFrontendAction::Disabled},
    {"MENU_GAME_OPTIONS", "GAME OPTIONS", false, VitaFrontendAction::Disabled},
    {"MENU_MULTIPLAYER_OPTIONS", "MULTIPLAYER OPTIONS...", false, VitaFrontendAction::Disabled},
    {"MENU_RESET_SYSTEM_DEFAULTS", "RESET SYSTEM DEFAULTS", false, VitaFrontendAction::Disabled},
    {"MENU_BACK", "BACK", true, VitaFrontendAction::Back},
};

constexpr VitaFrontendRouteItem kGraphicsRoute[] = {
    {"VITA_WORLD_RESOLUTION", "WORLD RESOLUTION", true, VitaFrontendAction::AdjustResolution},
    {"VITA_GRASS_DETAIL", "GRASS DETAIL", true, VitaFrontendAction::ToggleGrass},
#if KISAK_VITA_CPU_RUNTIME
    {"VITA_WORLD_LIGHTING", "WORLD LIGHTING", true, VitaFrontendAction::ToggleWorldLighting},
#endif
#if KISAK_VITA_BALANCED_CANDIDATE && KISAK_VITA_BENCHMARK
    {"VITA_COMPARISON", "RUN PERFORMANCE COMPARISON", true, VitaFrontendAction::RunComparison},
#endif
    {"MENU_BACK", "BACK", true, VitaFrontendAction::Back},
};
// Keep these test labels stable. Update this binding/help for the next GPU
// experiment instead of appending experiment-specific menu rows.
constexpr uint32_t kCurrentGpuDiagnostic = 37u;
constexpr const char* kCurrentGpuHelp = "140 SECONDS - SKY / LIGHTING / RESOLUTION";
constexpr VitaFrontendRouteItem kTestsRoute[] = {
#if KISAK_VITA_CPU_RUNTIME
    {"VITA_CPU_TEST", "CPU TEST", true, VitaFrontendAction::RunCpuDiagnostic},
    {"VITA_GPU_TEST", "GPU TEST", true, VitaFrontendAction::RunGpuTest},
    {"VITA_GRAPHICS_PROFILE", "GRAPHICS PROFILE", true, VitaFrontendAction::RunGraphicsProfile},
#else
    {"VITA_CPU_TEST", "CPU TEST", false, VitaFrontendAction::Disabled},
    {"VITA_GPU_TEST", "GPU TEST", true, VitaFrontendAction::RunShipmentTest},
#endif
};
constexpr VitaFrontendRouteItem kControlsRoute[] = {
    {"MENU_LOOK", "LOOK...", true, VitaFrontendAction::OpenControlsLook},
    {"MENU_MOVE", "MOVE...", false, VitaFrontendAction::Disabled},
    {"MENU_COMBAT", "COMBAT...", false, VitaFrontendAction::Disabled},
    {"MENU_INTERACT", "INTERACT...", false, VitaFrontendAction::Disabled},
    {"MENU_MULTIPLAYER_CONTROLS", "MULTIPLAYER CONTROLS...", false,
     VitaFrontendAction::Disabled},
    {"MENU_SET_DEFAULT_CONTROLS", "SET DEFAULT CONTROLS",
     true, VitaFrontendAction::ResetControls},
    {"MENU_BACK", "BACK", true, VitaFrontendAction::Back},
};

constexpr VitaFrontendRouteItem kControlsLookRoute[] = {
    // These keys/rows are taken directly from the authored options_look
    // menuDef.  Only the two settings backed by the Vita input bridge are
    // enabled; the remaining PC bindings stay present and visibly disabled.
    {"MENU_INVERT_MOUSE", "INVERT MOUSE", true,
     VitaFrontendAction::ToggleInvertY},
    {"MENU_LEAN_LEFT", "LEAN LEFT", false, VitaFrontendAction::Disabled},
    {"MENU_LEAN_RIGHT", "LEAN RIGHT", false, VitaFrontendAction::Disabled},
    {"MENU_LOOK_UP", "LOOK UP", false, VitaFrontendAction::Disabled},
    {"MENU_LOOK_DOWN", "LOOK DOWN", false, VitaFrontendAction::Disabled},
    {"MENU_TURN_LEFT", "TURN LEFT", false, VitaFrontendAction::Disabled},
    {"MENU_TURN_RIGHT", "TURN RIGHT", false, VitaFrontendAction::Disabled},
    {"MENU_MOUSE_LOOK", "MOUSE LOOK", false, VitaFrontendAction::Disabled},
    {"MENU_CENTER_VIEW", "CENTER VIEW", false, VitaFrontendAction::Disabled},
    {"MENU_FREE_LOOK", "FREE LOOK", false, VitaFrontendAction::Disabled},
    {"MENU_SMOOTH_MOUSE", "SMOOTH MOUSE", false, VitaFrontendAction::Disabled},
    {"MENU_MOUSE_SENSITIVITY", "MOUSE SENSITIVITY", true,
     VitaFrontendAction::AdjustSensitivity},
    {"MENU_BACK", "BACK", true, VitaFrontendAction::Back},
};

constexpr VitaFrontendRouteItem kPauseRoute[] = {
    {"MENU_RESUME", "RESUME GAME", true, VitaFrontendAction::ResumeGame},
    {"MENU_OPTIONS", "OPTIONS", true, VitaFrontendAction::OpenOptions},
    {"MENU_QUIT", "EXIT TO MENU", true, VitaFrontendAction::ExitToMain},
};

const VitaFrontendRouteItem* RouteItems(kisak::vita::VitaFrontendScreen screen,
                                        int* count) {
    if (count)
        *count = 0;
    switch (screen) {
    case kisak::vita::VitaFrontendScreen::Main:
        if (count) *count = static_cast<int>(std::size(kMainRoute));
        return kMainRoute;
    case kisak::vita::VitaFrontendScreen::Multiplayer:
        if (count) *count = static_cast<int>(std::size(kMultiplayerRoute));
        return kMultiplayerRoute;
    case kisak::vita::VitaFrontendScreen::GameModeSelect:
        if (count) *count = static_cast<int>(std::size(kGameModeRoute));
        return kGameModeRoute;
    case kisak::vita::VitaFrontendScreen::MapSelect:
        if (count) *count = static_cast<int>(std::size(kMapRoute));
        return kMapRoute;
    case kisak::vita::VitaFrontendScreen::Options:
        if (count) *count = static_cast<int>(std::size(kOptionsRoute));
        return kOptionsRoute;
    case kisak::vita::VitaFrontendScreen::Graphics:
        if (count) *count = static_cast<int>(std::size(kGraphicsRoute));
        return kGraphicsRoute;
    case kisak::vita::VitaFrontendScreen::Tests:
        if (count) *count = static_cast<int>(std::size(kTestsRoute));
        return kTestsRoute;
    case kisak::vita::VitaFrontendScreen::Controls:
        if (count) *count = static_cast<int>(std::size(kControlsRoute));
        return kControlsRoute;
    case kisak::vita::VitaFrontendScreen::ControlsLook:
        if (count) *count = static_cast<int>(std::size(kControlsLookRoute));
        return kControlsLookRoute;
    case kisak::vita::VitaFrontendScreen::Pause:
        if (count) *count = static_cast<int>(std::size(kPauseRoute));
        return kPauseRoute;
    default:
        return nullptr;
    }
}

void DrawRect(const GfxBackendApi* gfx, float left, float bottom, float right,
              float top, float depth, const float color[4], bool bindTexture = true) {
    if (!gfx || !color)
        return;
    GfxDrawVertex vertices[4]{};
    const float positions[4][2] = {
        {left, bottom}, {right, bottom}, {right, top}, {left, top},
    };
    for (uint32_t index = 0; index < 4; ++index) {
        vertices[index].position[0] = positions[index][0];
        vertices[index].position[1] = positions[index][1];
        vertices[index].position[2] = depth;
        std::memcpy(vertices[index].color, color, sizeof(vertices[index].color));
    }
    static const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    // Solid menu/panel quads are fixed-function draws.  Explicitly select
    // the untextured unit before submitting them so a preceding world,
    // lightmap, or font batch cannot leak its texture into the panel.  The
    // Vita backend also uses this call as the shader/auxiliary-unit reset
    // boundary when returning from the native source-lit path.
    if (bindTexture && gfx->BindTexture)
        gfx->BindTexture(GfxTextureHandle{0});
    gfx->DrawIndexed(vertices, 4, indices, 6);
}

void DrawTexturedRect(const GfxBackendApi* gfx, GfxTextureHandle texture,
                      float left, float bottom, float right, float top,
                      float depth, const float color[4]) {
    if (!gfx || texture.value == 0 || !gfx->BindTexture || !color)
        return;
    GfxDrawVertex vertices[4]{};
    const float positions[4][2] = {
        {left, bottom}, {right, bottom}, {right, top}, {left, top},
    };
    const float uvs[4][2] = {
        {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f},
    };
    for (uint32_t index = 0; index < 4; ++index) {
        vertices[index].position[0] = positions[index][0];
        vertices[index].position[1] = positions[index][1];
        vertices[index].position[2] = depth;
        vertices[index].texCoord[0] = uvs[index][0];
        vertices[index].texCoord[1] = uvs[index][1];
        std::memcpy(vertices[index].color, color, sizeof(vertices[index].color));
    }
    static const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    gfx->BindTexture(texture);
    gfx->DrawIndexed(vertices, 4, indices, 6);
    gfx->BindTexture(GfxTextureHandle{0});
}

// Match the desktop ScreenPlacement virtual 640x480 space used by the
// exported menuDef records.  The Vita's 960x544 framebuffer keeps the 4:3
// virtual viewport centered, so the menu never stretches across the wide
// framebuffer or shifts the authored button columns.
struct VitaMenuPlacement {
    static constexpr float kScaleX = 544.0f * (4.0f / 3.0f) / 640.0f;
    static constexpr float kScaleY = 544.0f / 480.0f;
    static constexpr float kViewportWidth = 960.0f;
    static constexpr float kViewportHeight = 544.0f;
    static constexpr float kSubScreenLeft =
        (kViewportWidth - 544.0f * (4.0f / 3.0f)) * 0.5f;

    static float ApplyX(float value, int32_t alignment) {
        switch (alignment) {
        case 1: return value * kScaleX;
        case 2: return value * kScaleX + kViewportWidth * 0.5f;
        case 3: return value * kScaleX + kViewportWidth;
        case 4: return value * (kViewportWidth / 640.0f);
        case 5: return value;
        case 6: return value * (640.0f / (544.0f * (4.0f / 3.0f)));
        case 7: return value * kScaleX + kViewportWidth * 0.5f;
        case 0:
        default: return value * kScaleX + kSubScreenLeft;
        }
    }

    static float ApplyY(float value, int32_t alignment) {
        switch (alignment) {
        case 1: return value * kScaleY;
        case 2: return value * kScaleY + kViewportHeight * 0.5f;
        case 3: return value * kScaleY + kViewportHeight;
        case 4: return value * (kViewportHeight / 480.0f);
        case 5: return value;
        case 6: return value * (480.0f / kViewportHeight);
        case 7: return value * kScaleY + kViewportHeight * 0.5f;
        case 0:
        default: return value * kScaleY;
        }
    }

    static float NdcX(float pixels) { return pixels * (2.0f / kViewportWidth) - 1.0f; }
    static float NdcY(float pixels) { return 1.0f - pixels * (2.0f / kViewportHeight); }
};

const kisak::vita::VitaMenuItem* FindRuntimeMenuItem(
    const kisak::vita::VitaMenuRecord* menu, const char* localizationKey) {
    if (!menu || !localizationKey || !*localizationKey)
        return nullptr;
    std::string token = "@";
    token += localizationKey;
    // The exported menu stream can contain a text-only heading followed by a
    // real button with the same localized label. Prefer the actionable item;
    // this keeps headings from accidentally supplying a zero-sized rectangle.
    const kisak::vita::VitaMenuItem* fallback = nullptr;
    for (const auto& item : menu->items) {
        if (item.text != token)
            continue;
        if (item.type != 0 && item.rect[2] > 0.0f && item.rect[3] > 0.0f)
            return &item;
        if (!fallback)
            fallback = &item;
    }
    return fallback && fallback->rect[2] > 0.0f && fallback->rect[3] > 0.0f
        ? fallback : nullptr;
}

const kisak::vita::VitaMenuRecord* RuntimeMenuForScreen(
    const kisak::vita::VitaMenuBundle* menus,
    kisak::vita::VitaFrontendScreen screen) {
    if (screen == kisak::vita::VitaFrontendScreen::Graphics
        || screen == kisak::vita::VitaFrontendScreen::Tests) return nullptr;
    if (!menus || !menus->Loaded())
        return nullptr;
    if (screen == kisak::vita::VitaFrontendScreen::Pause
        || screen == kisak::vita::VitaFrontendScreen::Loading
        || screen == kisak::vita::VitaFrontendScreen::InGame)
        return nullptr;
    const char* menuName = screen == kisak::vita::VitaFrontendScreen::Main
        ? "background_main"
        : screen == kisak::vita::VitaFrontendScreen::Multiplayer
            ? "main_controls"
            : screen == kisak::vita::VitaFrontendScreen::GameModeSelect
                ? "main_controls"
            : screen == kisak::vita::VitaFrontendScreen::Options
                ? "main_options" : "changemap";
    if (screen == kisak::vita::VitaFrontendScreen::Controls)
        menuName = "main_controls";
    else if (screen == kisak::vita::VitaFrontendScreen::ControlsLook)
        menuName = "options_look";
    return menus->Find(menuName);
}

// This mirrors ui_mp's default fontEnum (0) selection.  The PC code compares
// the real vertical scale against ui_smallFont=0.25, ui_bigFont=0.40, and
// ui_extraBigFont=0.55; the exported VITF records retain each font's authored
// glyph bearings, so selecting by scale is important for title/row parity.
const kisak::vita::VitaFontRecord* ResolveRetailFont(
    const kisak::vita::VitaFontBundle* fonts, float textScale,
    const kisak::vita::VitaFontRecord* fallback) {
    if (!fonts || !fonts->Loaded())
        return fallback;
    const float realScale = VitaMenuPlacement::kScaleY * textScale;
    const char* name = nullptr;
    if (realScale <= 0.25f) {
        name = "fonts/smallFont";
    } else if (realScale < 0.55f) {
        name = realScale < 0.40f ? "fonts/normalFont" : "fonts/bigFont";
    } else {
        name = "fonts/extraBigFont";
    }
    if (const auto* resolved = fonts->Find(name))
        return resolved;
    return fallback;
}

struct RetailMenuCommand {
    std::string verb;
    std::string argument;
};

bool ReadRetailQuoted(const std::string& script, size_t* offset,
                      std::string* value) {
    if (!offset || !value)
        return false;
    while (*offset < script.size()
           && (script[*offset] == ' ' || script[*offset] == '\t'
               || script[*offset] == '\r' || script[*offset] == '\n'
               || script[*offset] == ';')) {
        ++*offset;
    }
    if (*offset >= script.size() || script[*offset] != '"')
        return false;
    ++*offset;
    value->clear();
    while (*offset < script.size()) {
        const char character = script[(*offset)++];
        if (character == '"')
            return true;
        if (character == '\\' && *offset < script.size())
            value->push_back(script[(*offset)++]);
        else
            value->push_back(character);
    }
    return false;
}

std::vector<RetailMenuCommand> ParseRetailMenuAction(const std::string& script) {
    std::vector<RetailMenuCommand> commands;
    size_t offset = 0;
    while (offset < script.size()) {
        std::string verb;
        std::string argument;
        if (!ReadRetailQuoted(script, &offset, &verb))
            break;
        if (!ReadRetailQuoted(script, &offset, &argument))
            break;
        commands.push_back({std::move(verb), std::move(argument)});
    }
    return commands;
}

struct CustomClassDefinition {
    const char* prefix;
    const char* editorMenu;
    const char* slotKey;
    int baseStat;
};

constexpr CustomClassDefinition kCustomClassDefinitions[] = {
    {"assault", "menu_cac_assault", "CLASS_SLOT1", 200},
    {"specops", "menu_cac_specops", "CLASS_SLOT2", 210},
    {"heavygunner", "menu_cac_heavygunner", "CLASS_SLOT3", 220},
    {"demolitions", "menu_cac_demolitions", "CLASS_SLOT4", 230},
    {"sniper", "menu_cac_sniper", "CLASS_SLOT5", 240},
};

const CustomClassDefinition* CustomClassForEditorMenu(const std::string& menuName) {
    for (const CustomClassDefinition& definition : kCustomClassDefinitions) {
        if (menuName == definition.editorMenu)
            return &definition;
    }
    return nullptr;
}

const CustomClassDefinition* CustomClassForSlot(int slot) {
    return slot >= 0
        && slot < static_cast<int>(std::size(kCustomClassDefinitions))
        ? &kCustomClassDefinitions[slot] : nullptr;
}

int CustomClassSlotForEditorMenu(const std::string& menuName) {
    for (int index = 0; index < static_cast<int>(std::size(kCustomClassDefinitions)); ++index) {
        if (menuName == kCustomClassDefinitions[index].editorMenu)
            return index;
    }
    return -1;
}

bool StartsWith(const std::string& value, const char* prefix) {
    return prefix && value.rfind(prefix, 0) == 0;
}

std::string LowerAscii(std::string value) {
    for (char& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

int ClassTargetStatForMenu(const std::string& menuName,
                           const CustomClassDefinition* definition) {
    if (!definition)
        return 0;
    const std::string name = LowerAscii(menuName);
    const int base = definition->baseStat;
    if (name.find("defaultclasswarning") != std::string::npos
        || name.find("popup_cac_template") != std::string::npos)
        return 0;
    if (name.find("popup_cac_camo") != std::string::npos)
        return base + 9;
    if (name.find("popup_cac_perk1") != std::string::npos)
        return base + 5;
    if (name.find("popup_cac_perk2") != std::string::npos)
        return base + 6;
    if (name.find("popup_cac_perk3") != std::string::npos)
        return base + 7;
    if (name.find("popup_cac_extra") != std::string::npos)
        return base + 8;
    if (name.find("attachment_popup_") != std::string::npos)
        return base + (name.back() == '2' ? 4 : 2);
    if (name.find("popup_cac_secondary") != std::string::npos
        || name.find("popup_cac_primary2") != std::string::npos)
        return base + 3;
    if (name.find("popup_cac_primary") != std::string::npos
        || name.find("popup_cac_") != std::string::npos)
        return base + 1;
    return 0;
}

std::vector<std::string> SplitRetailSegments(const std::string& script) {
    std::vector<std::string> segments;
    size_t start = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t index = 0; index < script.size(); ++index) {
        const char character = script[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && character == ';') {
            segments.push_back(script.substr(start, index - start));
            start = index + 1;
        }
    }
    if (start < script.size())
        segments.push_back(script.substr(start));
    return segments;
}

std::vector<std::string> TokenizeRetailSegment(const std::string& segment) {
    std::vector<std::string> tokens;
    size_t index = 0;
    while (index < segment.size()) {
        while (index < segment.size()
               && (std::isspace(static_cast<unsigned char>(segment[index]))
                   || segment[index] == '(' || segment[index] == ')'
                   || segment[index] == ','))
            ++index;
        if (index >= segment.size())
            break;
        if (segment[index] == '"') {
            ++index;
            std::string token;
            while (index < segment.size()) {
                const char character = segment[index++];
                if (character == '"')
                    break;
                if (character == '\\' && index < segment.size())
                    token.push_back(segment[index++]);
                else
                    token.push_back(character);
            }
            tokens.push_back(std::move(token));
            continue;
        }
        const size_t start = index;
        while (index < segment.size()
               && !std::isspace(static_cast<unsigned char>(segment[index]))
               && segment[index] != '(' && segment[index] != ')'
               && segment[index] != ',')
            ++index;
        tokens.push_back(segment.substr(start, index - start));
    }
    return tokens;
}

bool ParseRetailInteger(const std::string& value, int32_t* result) {
    if (!result || value.empty())
        return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 0);
    if (end == value.c_str() || *end != '\0'
        || parsed < static_cast<long>(INT32_MIN)
        || parsed > static_cast<long>(INT32_MAX))
        return false;
    *result = static_cast<int32_t>(parsed);
    return true;
}

std::string RuntimeItemScript(const kisak::vita::VitaMenuItem& item) {
    return !item.onAccept.empty() ? item.onAccept : item.action;
}

bool ScriptHasQuotedCommand(const std::string& script, const char* command) {
    const std::string wanted = LowerAscii(command ? command : "");
    for (const std::string& segment : SplitRetailSegments(script)) {
        const std::vector<std::string> tokens = TokenizeRetailSegment(segment);
        for (const std::string& token : tokens) {
            if (LowerAscii(token) == wanted)
                return true;
        }
    }
    return false;
}

std::string ScriptOpenTarget(const std::string& script) {
    std::string target;
    for (const std::string& segment : SplitRetailSegments(script)) {
        const std::vector<std::string> tokens = TokenizeRetailSegment(segment);
        for (size_t index = 0; index < tokens.size(); ++index) {
            if (LowerAscii(tokens[index]) == "open" && index + 1 < tokens.size()) {
                target = tokens[index + 1];
                ++index;
            } else if (index + 4 < tokens.size()
                       && LowerAscii(tokens[index]) == "uiscript"
                       && LowerAscii(tokens[index + 1]) == "openmenuondvar") {
                target = tokens[index + 4];
                index += 4;
            }
        }
    }
    return target;
}

int ScriptFirstTableStat(const std::string& script) {
    for (const std::string& segment : SplitRetailSegments(script)) {
        const std::vector<std::string> tokens = TokenizeRetailSegment(segment);
        for (size_t index = 0; index + 1 < tokens.size(); ++index) {
            if (LowerAscii(tokens[index]) != "statsetusingtable")
                continue;
            int32_t stat = 0;
            if (ParseRetailInteger(tokens[index + 1], &stat))
                return stat;
        }
    }
    return 0;
}

bool IsClassMenuItemGeometry(const kisak::vita::VitaMenuItem& item) {
    return item.type != 0 && item.rect[2] > 0.0f && item.rect[3] > 0.0f
        && item.rect[2] <= 640.0f && item.rect[3] <= 480.0f
        && item.rect[0] >= -400.0f && item.rect[1] >= -400.0f;
}

bool IsOptionsMenuName(const std::string& name) {
    return name == "main_options" || name == "options_multi"
        || name == "controls_multi" || name.rfind("options_", 0) == 0;
}

// A deliberately tiny debug font.  It is not the COD4 font; it only makes
// the state-machine checkpoint legible on a Vita before Font_s/GfxImage
// conversion is complete.
const char* GlyphRow(char character,int row){return VitaDiagnosticGlyphRow(character,row);}

void DrawText(const GfxBackendApi* gfx, const char* text, float left, float top,
              float scale, const float color[4], float depth,
              const kisak::vita::VitaFontRecord* font = nullptr,
              GfxTextureHandle fontTexture = GfxTextureHandle{0},
              int32_t textStyle = 0) {
    if (!gfx || !text || !color)
        return;
    // ui_shared's menuDef stream uses ITEM_TEXTSTYLE_SHADOWEDMORE (6) for
    // headings and actionable rows. Keep the same physical offset in the
    // converted textured path; the shadow is submitted first so the real
    // glyph pass remains the source color and depth ordering.
    if (textStyle == 6 && font && fontTexture.value != 0) {
        // KisakCOD's text render flags map style 6 to renderFlags 0xC. Its
        // RB_DrawText path selects ofs=2 when both shadow bits are present;
        // express that same two-physical-pixel offset in framebuffer NDC.
        const float shadowColor[] = {0.0f, 0.0f, 0.0f, color[3]};
        const float shadowX = left + (4.0f / VitaMenuPlacement::kViewportWidth);
        const float shadowY = top - (4.0f / VitaMenuPlacement::kViewportHeight);
        DrawText(gfx, text, shadowX, shadowY, scale, shadowColor, depth,
                 font, fontTexture, 0);
    }
    if (font && fontTexture.value != 0 && gfx->BindTexture && !font->glyphs.empty()) {
        // `left`/`top` are already framebuffer NDC, while the glyph metrics
        // are authored in the PC font's pixel space.  The old implementation
        // divided by pixelHeight a second time, making the real VITF font
        // nearly invisible and leaving the fallback bitmap font to carry the
        // menus.  Convert authored pixels directly to NDC at the 960x544
        // framebuffer and preserve the retail textScale from the VITM item.
        // Match KisakCOD's R_NormalizedTextScale(font, scale): ui_mp passes
        // authored 48px-normalized values (0.40/0.55), while each converted
        // atlas record keeps its own pixelHeight (bigFont=24,
        // extraBigFont=48, etc.).  Omitting this factor made the row labels
        // half-size and bilinear filtering collapsed their thin strokes.
        const float normalizedScale = scale * 48.0f
            / static_cast<float>(std::max(1, font->pixelHeight));
        const float pixelsPerUnitX = VitaMenuPlacement::kScaleX * normalizedScale
            * (2.0f / VitaMenuPlacement::kViewportWidth);
        const float pixelsPerUnitY = VitaMenuPlacement::kScaleY * normalizedScale
            * (2.0f / VitaMenuPlacement::kViewportHeight);
        // DrawText2D initializes its pen at x - xScale * 0.5 before applying
        // each glyph's x0 bearing. Keep the same half-scale anchor in NDC;
        // this is a sub-pixel correction at Vita resolution but matters when
        // the authored row is rounded by the desktop renderer.
        float pen = left - 0.5f * pixelsPerUnitX;
        gfx->BindTexture(fontTexture);
        for (size_t characterIndex = 0; text[characterIndex]; ++characterIndex) {
            const uint16_t letter = static_cast<uint8_t>(text[characterIndex]);
            const auto glyphIt = std::find_if(font->glyphs.begin(), font->glyphs.end(),
                                              [letter](const kisak::vita::VitaFontGlyph& glyph) {
                                                  return glyph.letter == letter;
                                              });
            if (glyphIt == font->glyphs.end()) {
                pen += 4.0f * pixelsPerUnitX;
                continue;
            }
            const kisak::vita::VitaFontGlyph& glyph = *glyphIt;
            // The authored space glyph is advance-only (zero bitmap
            // dimensions).  Preserve its spacing but never submit a
            // zero-area textured quad: the scalar diagnostic rasterizer quite
            // reasonably collapses that quad to a one-pixel dot.
            if (glyph.pixelWidth == 0 || glyph.pixelHeight == 0) {
                pen += static_cast<float>(glyph.dx) * pixelsPerUnitX;
                continue;
            }
            const float x = pen + static_cast<float>(glyph.x0) * pixelsPerUnitX;
            // PC DrawText2D backs up only half the normalized yScale before
            // applying the glyph's signed y0 bearing. Subtracting half the
            // full pixelHeight here moves small labels into the row border.
            // NDC Y is inverted relative to the PC physical coordinate, so
            // subtracting half a pixel in PC space becomes an NDC addition.
            const float glyphAnchor = top + 0.5f * pixelsPerUnitY;
            // KisakCOD's DrawText2D starts at (y - yScale * .5) in physical
            // screen pixels, then adds the signed y0 bearing and the glyph
            // height to form the quad.  The frontend stores `top` as the
            // NDC coordinate of that physical text anchor and submits the
            // quad bottom-left first (the convention used by the software
            // sampler and the vitaGL path).  Therefore the NDC bottom is the
            // anchor plus half a scale, minus the bearing and glyph height.
            // Keep the authored bearing expression visible here because it is
            // the direct R_NormalizedTextScale/DrawText2D parity bridge.
            const float glyphBearing = glyphAnchor - static_cast<float>(glyph.y0) * pixelsPerUnitY;
            const float y = glyphBearing - pixelsPerUnitY
                * static_cast<float>(glyph.pixelHeight);
            const float width = static_cast<float>(glyph.pixelWidth) * pixelsPerUnitX;
            const float height = static_cast<float>(glyph.pixelHeight) * pixelsPerUnitY;
            GfxDrawVertex vertices[4]{};
            const float positions[4][2] = {
                {x, y}, {x + width, y}, {x + width, y + height}, {x, y + height},
            };
            const float uvs[4][2] = {
                {glyph.s0, glyph.t1}, {glyph.s1, glyph.t1},
                {glyph.s1, glyph.t0}, {glyph.s0, glyph.t0},
            };
            for (uint32_t index = 0; index < 4; ++index) {
                vertices[index].position[0] = positions[index][0];
                vertices[index].position[1] = positions[index][1];
                vertices[index].position[2] = depth;
                vertices[index].texCoord[0] = uvs[index][0];
                vertices[index].texCoord[1] = uvs[index][1];
                std::memcpy(vertices[index].color, color, sizeof(vertices[index].color));
            }
            static const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
            gfx->DrawIndexed(vertices, 4, indices, 6);
            pen += static_cast<float>(glyph.dx) * pixelsPerUnitX;
        }
        gfx->BindTexture(GfxTextureHandle{0});
        return;
    }
#if KISAK_VITA_CPU_RUNTIME
    // The fallback glyph run has one texture state. Establish it once, so
    // consecutive pixel quads can batch without changing layout or colour.
    if (gfx->BindTexture) gfx->BindTexture(GfxTextureHandle{0});
#endif
    const float pixelWidth = 0.009f * scale;
    const float pixelHeight = 0.014f * scale;
    const float glyphAdvance = 0.060f * scale;
    for (size_t characterIndex = 0; text[characterIndex]; ++characterIndex) {
        const char* character = text + characterIndex;
        for (int row = 0; row < 7; ++row) {
            const char* bits = GlyphRow(*character, row);
            for (int column = 0; column < 5; ++column) {
                if (bits[column] != '1')
                    continue;
                const float x = left + static_cast<float>(characterIndex) * glyphAdvance
                    + static_cast<float>(column) * pixelWidth;
                const float y = top - static_cast<float>(row + 1) * pixelHeight;
                DrawRect(gfx, x, y, x + pixelWidth * 0.86f, y + pixelHeight * 0.86f,
                         depth, color
#if KISAK_VITA_CPU_RUNTIME
                         , false
#endif
                         );
            }
        }
    }
}

// The retail localization table represents the Cross/Circle controls with
// platform glyph bytes (MENU_R_CIRCLE and its companion select token).  Those
// bytes are intentionally not present in the PC font atlas exported for the
// Vita package, so drawing the literal words "CROSS"/"CIRCLE" is visibly
// unlike the COD4 footer.  Keep the labels in the authored font, but draw the
// two platform marks as tiny allocation-free geometry at the same baseline.
void DrawFooterCross(const GfxBackendApi* gfx, float centerX, float centerY,
                     float depth, const float color[4]) {
    if (!gfx || !color)
        return;
    constexpr float kSpan = 0.011f;
    constexpr float kThickness = 0.0025f;
    DrawRect(gfx, centerX - kSpan, centerY - kThickness,
             centerX + kSpan, centerY + kThickness, depth, color);
    DrawRect(gfx, centerX - kThickness, centerY - kSpan,
             centerX + kThickness, centerY + kSpan, depth, color);
}

void DrawFooterCircle(const GfxBackendApi* gfx, float centerX, float centerY,
                      float depth, const float color[4]) {
    if (!gfx || !color)
        return;
    // Eight short axis-aligned segments are enough at the Vita footer's
    // authored size and avoid introducing a line primitive to the backend.
    constexpr float kRadius = 0.010f;
    constexpr float kThickness = 0.0025f;
    constexpr float kInner = 0.0065f;
    DrawRect(gfx, centerX - kInner, centerY + kRadius - kThickness,
             centerX + kInner, centerY + kRadius, depth, color);
    DrawRect(gfx, centerX - kInner, centerY - kRadius,
             centerX + kInner, centerY - kRadius + kThickness, depth, color);
    DrawRect(gfx, centerX - kRadius, centerY - kInner,
             centerX - kRadius + kThickness, centerY + kInner, depth, color);
    DrawRect(gfx, centerX + kRadius - kThickness, centerY - kInner,
             centerX + kRadius, centerY + kInner, depth, color);
    DrawRect(gfx, centerX - kInner - kThickness, centerY + kInner,
             centerX - kInner, centerY + kInner + kThickness, depth, color);
    DrawRect(gfx, centerX + kInner, centerY + kInner,
             centerX + kInner + kThickness, centerY + kInner + kThickness, depth, color);
    DrawRect(gfx, centerX - kInner - kThickness, centerY - kInner - kThickness,
             centerX - kInner, centerY - kInner, depth, color);
    DrawRect(gfx, centerX + kInner, centerY - kInner - kThickness,
             centerX + kInner + kThickness, centerY - kInner, depth, color);
}

const char* ResolveLocalizedTitle(
    const kisak::vita::VitaLocalizeBundle* localized, const char* key,
    const char* fallback, std::string* storage) {
    if (localized && key && storage) {
        if (const auto* entry = localized->Find(key)) {
            *storage = entry->value;
            if (!storage->empty())
                return storage->c_str();
        }
    }
    return fallback;
}

void DrawRetailMenuTitle(const GfxBackendApi* gfx, const char* title,
                         const kisak::vita::VitaFontRecord* font,
                         GfxTextureHandle fontTexture) {
    if (!gfx || !title)
        return;
    const float titleColor[] = {0.90f, 0.93f, 1.0f, 1.0f};
    const float accent[] = {0.20f, 0.65f, 1.0f, 1.0f};
    // The retail menu headings live in the centered 640x480 virtual space at
    // roughly (48,40); keep the same baseline and only retain a thin accent
    // rule as the authored menu's visual separator.
    // The recovered menu headings use the default horizontal placement (the
    // centered 4:3 viewport supplies the safe-area gutter).  Treating the
    // 48px authored x coordinate as a second center alignment pushed the
    // title into the right edge of the 960px framebuffer.
    const float titleX = VitaMenuPlacement::NdcX(
        VitaMenuPlacement::ApplyX(48.0f, 0));
    const float titleY = VitaMenuPlacement::NdcY(
        VitaMenuPlacement::ApplyY(40.0f, 0));
    // The exported COD4 title item uses 0.5833 textScale (extraBigFont),
    // not the temporary oversized diagnostic scale that clipped its glyphs
    // against the top edge after the source y-bearing was restored.
    DrawText(gfx, title, titleX, titleY, 0.5833f, titleColor, 0.38f,
             font, fontTexture, 6);
    const float ruleLeft = VitaMenuPlacement::NdcX(
        VitaMenuPlacement::ApplyX(-324.0f, 2));
    const float ruleRight = VitaMenuPlacement::NdcX(
        VitaMenuPlacement::ApplyX(-104.0f, 2));
    const float ruleTop = VitaMenuPlacement::NdcY(
        VitaMenuPlacement::ApplyY(66.0f, 0));
    DrawRect(gfx, ruleLeft, ruleTop - 0.004f, ruleRight, ruleTop,
             0.39f, accent);
}

void DrawMenuItems(const GfxBackendApi* gfx, const VitaFrontendRouteItem* items,
                   int count, int selection,
                   const kisak::vita::VitaMenuRecord* menu,
                   const kisak::vita::VitaLocalizeBundle* localized,
                   GfxTextureHandle menuButtonTexture,
                   GfxTextureHandle menuButtonSelectedTexture,
                   const kisak::vita::VitaFontBundle* fonts,
                   const kisak::vita::VitaFontRecord* font,
                   GfxTextureHandle fontTexture, bool compactMapRoute = false) {
    // ui_shared paints every menu item inside its authored window. The
    // converted menu-button atlases carry the same translucent grey gradient
    // used by COD4's console frontend; tinting them rather than replacing
    // them with a solid blue strip preserves that source presentation on
    // Vita. Keep a solid fallback for assets that are unavailable during a
    // diagnostic boot.
    const float selectedFallback[] = {0.88f, 0.91f, 0.96f, 0.84f};
    const float normalButtonColor[] = {0.72f, 0.76f, 0.82f, 0.66f};
    const float disabledButtonColor[] = {0.42f, 0.45f, 0.50f, 0.48f};
    const float textColor[] = {0.92f, 0.94f, 1.0f, 1.0f};
    const float disabledTextColor[] = {0.63f, 0.66f, 0.71f, 0.92f};
    // changemap's source listbox contains every base-game map.  The Vita
    // capability manifest keeps those disabled rows visible, so use a
    // compact authored-style list that fits beside the preview instead of
    // clipping the tail of the list below the 960x544 viewport.
    const bool mapRoute = compactMapRoute || items == kMapRoute;
    const float row = mapRoute ? 0.071f : (count > 8 ? 0.105f : 0.13f);
    const float firstRowTop = mapRoute ? 0.79f : (count > 8 ? 0.61f : 0.54f);
    // main_options is authored with seven consecutive 220x22 rows at
    // y=34..178.  The Vita capability manifest adds the supported Controls
    // entry to that same retail list; keep the source grid (rather than
    // mixing one fallback row with the unshifted authored rows) so the extra
    // row advances every category by the same 24px PC spacing.
    const bool optionsRouteWithControls = menu
        && menu->name == "main_options" && items == kOptionsRoute;
    for (int index = 0; index < count; ++index) {
        int32_t itemTextStyle = 6;
        // The exported MENU_BACK record is a top-edge helper used by the
        // desktop script menu, while this Vita route already owns the footer
        // Back token. Keep the row selectable, but use the same list geometry
        // as the other route items so the helper cannot appear clipped at the
        // top of Multiplayer/Map Select captures.
        const bool backRouteItem = items[index].localizationKey
            && std::strcmp(items[index].localizationKey, "MENU_BACK") == 0;
        // Script-driven main-menu routes do not have a dedicated menuDef in
        // the exported pool.  Reuse the same 220x22 virtual action row that
        // the authored main_controls/main_options/changemap definitions use
        // instead of stretching a compatibility row across the whole screen.
        const float authoredLeft = VitaMenuPlacement::NdcX(
            VitaMenuPlacement::ApplyX(-324.0f, 2));
        const float authoredRight = VitaMenuPlacement::NdcX(
            VitaMenuPlacement::ApplyX(-104.0f, 2));
        float left = authoredLeft;
        float bottom = firstRowTop - static_cast<float>(index) * row - 0.082f;
        float right = authoredRight;
        float top = firstRowTop - static_cast<float>(index) * row + 0.016f;
        float textLeft = -0.70f;
        float textTop = top - 0.078f;
        // Script-only routes still use the same 0.40 default menu text scale
        // as the authored ui_mp rows.  The old 0.82 fallback made the Vita
        // labels materially larger than PC until a menuDef happened to match.
        float textScale = 0.40f;
        if (optionsRouteWithControls) {
            constexpr int32_t kMenuHorzAlign = 2;
            constexpr int32_t kMenuVertAlign = 0;
            const float itemLeftPx = VitaMenuPlacement::ApplyX(
                -324.0f, kMenuHorzAlign);
            const float itemTopPx = VitaMenuPlacement::ApplyY(
                34.0f + static_cast<float>(index) * 24.0f, kMenuVertAlign);
            const float itemWidthPx = 220.0f * VitaMenuPlacement::kScaleX;
            const float itemHeightPx = 22.0f * VitaMenuPlacement::kScaleY;
            left = std::clamp(VitaMenuPlacement::NdcX(itemLeftPx),
                              -0.98f, 0.96f);
            right = std::clamp(VitaMenuPlacement::NdcX(itemLeftPx + itemWidthPx),
                               left + 0.04f, 0.98f);
            top = std::clamp(VitaMenuPlacement::NdcY(itemTopPx),
                             -0.96f, 0.98f);
            bottom = std::clamp(
                VitaMenuPlacement::NdcY(itemTopPx + itemHeightPx),
                -0.98f, top - 0.02f);
            textLeft = std::clamp(left + 0.045f, -0.94f, 0.92f);
            // DrawText receives the same baseline-like anchor that
            // ui_shared's DrawText2D receives. The exported bigFont has a
            // negative y-bearing, so using the row's top edge directly puts
            // the glyph quad above its 22px window. Center the retail glyph
            // in that window (about 20 Vita pixels below the anchor).
            textTop = top - 0.078f;
            textScale = 0.40f;
        } else if (!backRouteItem) {
            if (const auto* menuItem = FindRuntimeMenuItem(
                    menu, items[index].localizationKey)) {
            // These route items are authored with the retail center/top
            // anchors in ui_mp.  Apply the same ScreenPlacement transform as
            // the PC build instead of treating 640x480 values as already
            // normalized (which stretched the menu into the 16:9 gutters).
            constexpr int32_t kMenuHorzAlign = 2;
            constexpr int32_t kMenuVertAlign = 0;
            const float itemLeftPx = VitaMenuPlacement::ApplyX(
                menuItem->rect[0], kMenuHorzAlign);
            const float itemTopPx = VitaMenuPlacement::ApplyY(
                menuItem->rect[1], kMenuVertAlign);
            const float itemWidthPx = menuItem->rect[2] * VitaMenuPlacement::kScaleX;
            const float itemHeightPx = menuItem->rect[3] * VitaMenuPlacement::kScaleY;
            const float itemLeft = VitaMenuPlacement::NdcX(itemLeftPx);
            const float itemTop = VitaMenuPlacement::NdcY(itemTopPx);
            const float itemRight = VitaMenuPlacement::NdcX(itemLeftPx + itemWidthPx);
            const float itemBottom = VitaMenuPlacement::NdcY(itemTopPx + itemHeightPx);
            left = std::clamp(itemLeft, -0.98f, 0.96f);
            right = std::clamp(itemRight, left + 0.04f, 0.98f);
            top = std::clamp(itemTop, -0.96f, 0.98f);
            bottom = std::clamp(itemBottom, -0.98f, top - 0.02f);
            textLeft = std::clamp(left + 0.045f, -0.94f, 0.92f);
            textTop = top - 0.078f;
            textScale = std::clamp(menuItem->textScale, 0.30f, 0.90f);
                itemTextStyle = menuItem->textStyle;
            }
        }
        const bool selectedRow = index == selection && items[index].enabled;
        const float* rowTextColor = selectedRow ? textColor
            : (items[index].enabled ? textColor : disabledTextColor);
        const float* rowColor = selectedRow ? selectedFallback
            : (items[index].enabled ? normalButtonColor : disabledButtonColor);
        // Source menuDef items all paint their window, including disabled
        // rows. Drawing the normal atlas for those rows keeps the labels
        // contained by the same grey gradient instead of leaving unsupported
        // options floating over the background.
        const GfxTextureHandle rowTexture = selectedRow
            && menuButtonSelectedTexture.value != 0
            ? menuButtonSelectedTexture : menuButtonTexture;
        if (rowTexture.value != 0) {
            DrawTexturedRect(gfx, rowTexture, left, bottom, right, top,
                             selectedRow ? 0.30f : 0.31f, rowColor);
        } else {
            DrawRect(gfx, left, bottom, right, top, 0.30f, rowColor);
        }
        if (selectedRow) {
            // The retail focus window is a light translucent wash over the
            // normal gradient, not a detached blue bar. Keep the authored
            // selected atlas underneath and add only its subtle focus fill.
            const float focusWash[] = {0.92f, 0.94f, 0.98f, 0.16f};
            DrawRect(gfx, left, bottom, right, top, 0.295f, focusWash);
        }
        const char* label = items[index].fallbackLabel;
        std::string localizedLabel;
        if (localized && items[index].localizationKey) {
            if (const auto* entry = localized->Find(items[index].localizationKey)) {
                localizedLabel = entry->value;
                label = localizedLabel.c_str();
            }
        }
        // Keep unsupported rows grey even if an input edge briefly reports a
        // stale selection index.  The retail UI resolves disabled text through
        // the menu's disableColor after focus handling; it never promotes a
        // disabled label to the white normal-text color.
        DrawText(gfx, label, textLeft, textTop, textScale,
                 rowTextColor, 0.28f,
                 ResolveRetailFont(fonts, textScale, font), fontTexture,
                 itemTextStyle);
    }
}

void DrawAuthoredClassMenu(
    const GfxBackendApi* gfx, const kisak::vita::VitaMenuRecord* menu,
    const std::vector<int>& indices, const std::vector<std::string>& labels,
    int selection, GfxTextureHandle normalTexture,
    GfxTextureHandle selectedTexture, const kisak::vita::VitaFontBundle* fonts,
    const kisak::vita::VitaFontRecord* fallbackFont,
    GfxTextureHandle fontTexture) {
    if (!gfx || !menu)
        return;
    const float selectedFallback[] = {0.88f, 0.91f, 0.96f, 0.84f};
    const float normalButtonColor[] = {0.72f, 0.76f, 0.82f, 0.66f};
    const float textColor[] = {0.92f, 0.94f, 1.0f, 1.0f};
    for (size_t row = 0; row < indices.size() && row < labels.size(); ++row) {
        const int itemIndex = indices[row];
        if (itemIndex < 0 || itemIndex >= static_cast<int>(menu->items.size()))
            continue;
        const kisak::vita::VitaMenuItem& item = menu->items[itemIndex];
        constexpr int32_t kMenuHorzAlign = 2;
        constexpr int32_t kMenuVertAlign = 0;
        const float itemLeftPx = VitaMenuPlacement::ApplyX(item.rect[0], kMenuHorzAlign);
        const float itemTopPx = VitaMenuPlacement::ApplyY(item.rect[1], kMenuVertAlign);
        const float itemRightPx = itemLeftPx + item.rect[2] * VitaMenuPlacement::kScaleX;
        const float itemBottomPx = itemTopPx + item.rect[3] * VitaMenuPlacement::kScaleY;
        const float left = std::clamp(VitaMenuPlacement::NdcX(itemLeftPx), -0.98f, 0.96f);
        const float right = std::clamp(VitaMenuPlacement::NdcX(itemRightPx), left + 0.04f, 0.98f);
        const float top = std::clamp(VitaMenuPlacement::NdcY(itemTopPx), -0.96f, 0.98f);
        const float bottom = std::clamp(VitaMenuPlacement::NdcY(itemBottomPx), -0.98f, top - 0.02f);
        const bool selected = static_cast<int>(row) == selection;
        const float* rowColor = selected ? selectedFallback : normalButtonColor;
        const GfxTextureHandle texture = selected && selectedTexture.value != 0
            ? selectedTexture : normalTexture;
        if (texture.value != 0)
            DrawTexturedRect(gfx, texture, left, bottom, right, top,
                             selected ? 0.30f : 0.31f, rowColor);
        else
            DrawRect(gfx, left, bottom, right, top, 0.30f, rowColor);
        if (selected) {
            const float focusWash[] = {0.92f, 0.94f, 0.98f, 0.16f};
            DrawRect(gfx, left, bottom, right, top, 0.295f, focusWash);
        }
        const float textScale = std::clamp(item.textScale, 0.30f, 0.90f);
        const float textLeft = std::clamp(left + 0.045f, -0.94f, 0.92f);
        const float textTop = top - 0.078f;
        DrawText(gfx, labels[row].c_str(), textLeft, textTop, textScale,
                 textColor, 0.28f, ResolveRetailFont(fonts, textScale,
                                                     fallbackFont),
                 fontTexture, item.textStyle);
    }
}

} // namespace

namespace kisak::vita {

namespace {

bool IsSelectable(VitaFrontendScreen screen, int index,
                  uint32_t availableMapMask) {
    int count = 0;
    const VitaFrontendRouteItem* items = RouteItems(screen, &count);
    if (!items || index < 0 || index >= count)
        return true;
    if (!items[index].enabled)
        return false;
    return screen != VitaFrontendScreen::MapSelect
        || items[index].action==VitaFrontendAction::Back
        || (index < 32 && (availableMapMask & (1u << index)) != 0);
}

int FirstSelectable(VitaFrontendScreen screen, uint32_t availableMapMask) {
    int count = 0;
    const VitaFrontendRouteItem* items = RouteItems(screen, &count);
    if (!items)
        return 0;
    for (int index = 0; index < count; ++index) {
        if (IsSelectable(screen, index, availableMapMask))
            return index;
    }
    return 0;
}

} // namespace

bool VitaFrontend::ExecuteRetailMenuAction(int routeIndex) {
    int count = 0;
    const VitaFrontendRouteItem* items = RouteItems(screen_, &count);
    if (!items || routeIndex < 0 || routeIndex >= count
        || !IsSelectable(screen_, routeIndex, availableMapMask_))
        return false;
    if (items[routeIndex].action == VitaFrontendAction::OpenCustomClasses) {
        activeMenuName_ = "pc_cac_popup_ingame";
        classMenuHistory_.clear();
        classSelectionTargetStat_ = 0;
        screen_ = VitaFrontendScreen::CustomClasses;
        selection_ = 0;
        return true;
    }
    // The retail changemap list is populated by the MP map table rather than
    // by one hardcoded action string per row. Preserve that behavior here so
    // every authored map row enters the same load transition and the
    // bootstrap can select the matching world asset ids.
    if (screen_ == VitaFrontendScreen::MapSelect
        && items[routeIndex].action == VitaFrontendAction::StartShipment
        && routeIndex >= 0
        && routeIndex < static_cast<int>(kisak::vita::kVitaMpMapCount)) {
        selectedMapIndex_ = static_cast<uint32_t>(routeIndex);
        screen_ = VitaFrontendScreen::Loading;
        loadingFrames_ = 45;
        loadingReady_ = false;
    SetLoadingStage("", 0, 0);
        return true;
    }
    if (!menus_)
        return false;
    const VitaMenuRecord* menu = RuntimeMenuForScreen(menus_, screen_);
    const VitaMenuItem* item = FindRuntimeMenuItem(
        menu, items[routeIndex].localizationKey);
    if (!item)
        return false;
    const std::string& script = !item->onAccept.empty()
        ? item->onAccept : item->action;
    if (script.empty())
        return false;

    // Retail menuDef actions are a sequence of quoted command/argument pairs.
    // Execute only the local navigation subset; commands such as vote/cmd are
    // deliberately left to the bounded capability manifest and never reach a
    // network or filesystem side effect on Vita.
    const std::vector<RetailMenuCommand> commands = ParseRetailMenuAction(script);
    if (commands.empty())
        return false;
    std::string lastOpen;
    bool closeSelf = false;
    bool recognized = false;
    bool unsafe = false;
    for (const RetailMenuCommand& command : commands) {
        if (command.verb == "play") {
            recognized = true;
        } else if (command.verb == "close" && command.argument == "self") {
            closeSelf = true;
            recognized = true;
        } else if (command.verb == "exec" && command.argument == "wait") {
            recognized = true;
        } else if (command.verb == "open") {
            lastOpen = command.argument;
            recognized = true;
        } else if (command.verb == "cmd"
                   || (command.verb == "exec"
                       && command.argument.rfind("cmd ", 0) == 0)) {
            // Keep the offline slice from inheriting retail vote/network or
            // arbitrary UI-script side effects, even when the same action
            // string also contains a harmless close/open pair.
            unsafe = true;
        }
    }
    if (!recognized || unsafe)
        return false;
    if (lastOpen == "changemap") {
        mapReturnScreen_ = screen_;
        screen_ = VitaFrontendScreen::MapSelect;
        selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    }
    if (lastOpen == "main_controls") {
        // In the recovered ui_mp stream, main_controls is the actual COD4
        // controls page (the multiplayer/team menus only open it from their
        // in-game controls row).  Do not route this authored action back to
        // the Multiplayer capability manifest; that made a real MENU_CONTROLS
        // action appear to jump sideways into the lobby.
        screen_ = VitaFrontendScreen::Controls;
        selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    }
    // The retail action opens a confirmation popup that is not yet part of
    // the bounded Vita menu bundle.  Let the route action below apply the
    // same default-controls result without exposing a dead popup transition.
    if (lastOpen == "options_control_defaults")
        return false;
    if (screen_ == VitaFrontendScreen::Controls
        && lastOpen == "options_look") {
        screen_ = VitaFrontendScreen::ControlsLook;
        selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    }
    if (IsOptionsMenuName(lastOpen)) {
        optionsReturnScreen_ = screen_ == VitaFrontendScreen::Multiplayer
            ? VitaFrontendScreen::Multiplayer : VitaFrontendScreen::Main;
        screen_ = VitaFrontendScreen::Options;
        selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    }
    if (closeSelf) {
        Back();
        return true;
    }
    return false;
}

bool VitaFrontend::IsClassScreen() const {
    return screen_ == VitaFrontendScreen::CustomClasses
        || screen_ == VitaFrontendScreen::ClassEditor
        || screen_ == VitaFrontendScreen::ClassPicker;
}

const VitaMenuRecord* VitaFrontend::ActiveClassMenu() const {
    if (!menus_ || !menus_->Loaded() || activeMenuName_.empty())
        return nullptr;
    return menus_->Find(activeMenuName_.c_str());
}

void VitaFrontend::ResetCustomClassStats() {
    customClassStats_[0] = {0, 0x19, 5, 0, 0, 0xBF, 0xA0, 0x9A, 0x67, 0};
    customClassStats_[1] = {0, 0x0A, 0, 2, 3, 0xB8, 0x9C, 0xA2, 0x65, 0};
    customClassStats_[2] = {0, 0x51, 0, 2, 0, 0xB0, 0xA7, 0xA1, 0x67, 0};
    customClassStats_[3] = {0, 0x47, 0, 0, 0, 0xBA, 0x9C, 0x9A, 0x66, 0};
    customClassStats_[4] = {0, 0x3D, 0, 0, 3, 0xB0, 0xA0, 0xA1, 0x65, 0};
    for (size_t index = 0; index < customClassNames_.size(); ++index) {
        customClassNames_[index] = CustomClassForSlot(static_cast<int>(index))->slotKey;
        customClassWeaponKeys_[index][0].clear();
        customClassWeaponKeys_[index][1].clear();
    }
}

void VitaFrontend::SetCustomClassStat(int32_t statIndex, int32_t value) {
    if (statIndex < 200 || statIndex >= 250)
        return;
    const int slot = statIndex / 10 - 20;
    const int offset = statIndex % 10;
    if (slot < 0 || slot >= static_cast<int>(customClassStats_.size())
        || offset < 0 || offset >= static_cast<int>(kClassStatCount))
        return;
    customClassStats_[slot][offset] = value;
    if ((offset == 1 || offset == 3) && value == 0)
        customClassWeaponKeys_[slot][offset == 3 ? 1 : 0].clear();
}

int32_t VitaFrontend::CustomClassStat(uint32_t slot, uint32_t offset) const {
    if (slot >= customClassStats_.size() || offset >= kClassStatCount)
        return 0;
    return customClassStats_[slot][offset];
}

const char* VitaFrontend::CustomClassWeaponKey(uint32_t slot, bool secondary) const {
    if (slot >= customClassWeaponKeys_.size())
        return nullptr;
    const std::string& key = customClassWeaponKeys_[slot][secondary ? 1 : 0];
    return key.empty() ? nullptr : key.c_str();
}

std::vector<int> VitaFrontend::ClassMenuItemIndices() const {
    std::vector<int> indices;
    const VitaMenuRecord* menu = ActiveClassMenu();
    if (!menu)
        return indices;

    const bool classList = menu->name == "pc_cac_popup_ingame";
    const bool editor = CustomClassForEditorMenu(menu->name) != nullptr;
    std::vector<std::string> labels;
    for (int index = 0; index < static_cast<int>(menu->items.size()); ++index) {
        const VitaMenuItem& item = menu->items[index];
        if (!IsClassMenuItemGeometry(item))
            continue;
        const std::string script = RuntimeItemScript(item);
        if (script.empty())
            continue;
        const std::string target = ScriptOpenTarget(script);
        const bool hasOpen = !target.empty();
        const bool hasClose = ScriptHasQuotedCommand(script, "close");
        const bool hasTable = ScriptHasQuotedCommand(script, "statsetusingtable");
        if (classList) {
            if (!StartsWith(target, "menu_cac_"))
                continue;
        } else if (editor) {
            if (item.text.empty() || (!hasOpen && !hasClose && !hasTable))
                continue;
        } else if (item.text.empty() || (!hasOpen && !hasClose && !hasTable)) {
            continue;
        }

        // The retail menu has separate visibility branches serialized as
        // duplicate rows. Select the branch whose source stat index matches
        // the active primary/secondary target, then keep one visible label.
        const int tableStat = ScriptFirstTableStat(script);
        const bool targetMatches = classSelectionTargetStat_ == 0
            || tableStat == 0 || tableStat == classSelectionTargetStat_;
        if (!targetMatches)
            continue;
        const std::string label = item.text.empty() ? target : item.text;
        const auto existing = std::find(labels.begin(), labels.end(), label);
        if (existing != labels.end())
            continue;
        labels.push_back(label);
        indices.push_back(index);
    }

    // A target-specific branch can be absent in an older/reference menu
    // stream. In that case retain the first authored row for each label so
    // the menu remains navigable without inventing an option list.
    if (indices.empty() && menu->name != "pc_cac_popup_ingame") {
        for (int index = 0; index < static_cast<int>(menu->items.size()); ++index) {
            const VitaMenuItem& item = menu->items[index];
            if (!IsClassMenuItemGeometry(item) || item.text.empty())
                continue;
            const std::string script = RuntimeItemScript(item);
            if (script.empty() || (!ScriptHasQuotedCommand(script, "open")
                                   && !ScriptHasQuotedCommand(script, "close")
                                   && !ScriptHasQuotedCommand(script, "statsetusingtable")))
                continue;
            if (std::find(labels.begin(), labels.end(), item.text) != labels.end())
                continue;
            labels.push_back(item.text);
            indices.push_back(index);
        }
    }
    return indices;
}

bool VitaFrontend::ExecuteClassMenuScript(const std::string& script) {
    if (script.empty())
        return false;
    bool recognized = false;
    bool unsafe = false;
    bool closeSelf = false;
    bool namedClose = false;
    std::string lastOpen;
    std::unordered_map<std::string, std::string> dvarValues;
    const CustomClassDefinition* definition =
        CustomClassForSlot(activeCustomClassSlot_);

    std::function<void(const std::string&)> execute = [&](const std::string& source) {
        for (const std::string& segment : SplitRetailSegments(source)) {
            const std::vector<std::string> tokens = TokenizeRetailSegment(segment);
            for (size_t index = 0; index < tokens.size(); ++index) {
                const std::string command = LowerAscii(tokens[index]);
                if (command == "play") {
                    recognized = true;
                } else if (command == "open" && index + 1 < tokens.size()) {
                    lastOpen = tokens[++index];
                    recognized = true;
                } else if (command == "close" && index + 1 < tokens.size()) {
                    const std::string target = LowerAscii(tokens[++index]);
                    if (target == "self")
                        closeSelf = true;
                    else
                        namedClose = true;
                    recognized = true;
                } else if (command == "statset" && index + 2 < tokens.size()) {
                    int32_t statIndex = 0;
                    int32_t value = 0;
                    if (ParseRetailInteger(tokens[index + 1], &statIndex)
                        && ParseRetailInteger(tokens[index + 2], &value)) {
                        SetCustomClassStat(statIndex, value);
                        recognized = true;
                    }
                    index += 2;
                } else if (command == "statsetusingtable" && index + 6 < tokens.size()
                           && LowerAscii(tokens[index + 2]) == "tablelookup") {
                    int32_t statIndex = 0;
                    int32_t comparisonColumn = 0;
                    int32_t valueColumn = 0;
                    if (ParseRetailInteger(tokens[index + 1], &statIndex)
                        && ParseRetailInteger(tokens[index + 4], &comparisonColumn)
                        && ParseRetailInteger(tokens[index + 6], &valueColumn)) {
                        std::string value;
                        const bool found = stringTables_
                            && stringTables_->Lookup(tokens[index + 3].c_str(),
                                                     comparisonColumn,
                                                     tokens[index + 5].c_str(),
                                                     valueColumn, &value);
                        if (found) {
                            int32_t parsedValue = 0;
                            if (ParseRetailInteger(value, &parsedValue))
                                SetCustomClassStat(statIndex, parsedValue);
                            if (LowerAscii(tokens[index + 3]) == "mp/statstable.csv"
                                && definition && (statIndex == definition->baseStat + 1
                                                   || statIndex == definition->baseStat + 3)) {
                                customClassWeaponKeys_[activeCustomClassSlot_]
                                    [statIndex == definition->baseStat + 3 ? 1 : 0]
                                    = tokens[index + 5];
                            }
                        }
                        recognized = true;
                    }
                    index += 6;
                } else if (command == "exec" || command == "execnow") {
                    recognized = true;
                    if (index + 1 < tokens.size()) {
                        const std::string argument = tokens[++index];
                        if (LowerAscii(argument).rfind("cmd ", 0) == 0)
                            unsafe = true;
                        else if (command == "execnow"
                                 || LowerAscii(argument).rfind("statset ", 0) == 0
                                 || LowerAscii(argument).rfind("set ", 0) == 0)
                            execute(argument);
                    }
                } else if (command == "uiscript") {
                    recognized = true;
                    if (index + 4 < tokens.size()
                        && LowerAscii(tokens[index + 1]) == "openmenuondvar"
                        && LowerAscii(tokens[index + 2]) == "selected_weapon_class") {
                        const std::string candidateClass = tokens[index + 3];
                        if (selectedWeaponClass_.empty()
                            || LowerAscii(selectedWeaponClass_) == LowerAscii(candidateClass)) {
                            selectedWeaponClass_ = candidateClass;
                            lastOpen = tokens[index + 4];
                        }
                        index += 4;
                    }
                } else if (command == "set" && index + 2 < tokens.size()) {
                    recognized = true;
                    const std::string dvar = LowerAscii(tokens[index + 1]);
                    dvarValues[dvar] = tokens[index + 2];
                    if (dvar == "selected_weapon_class")
                        selectedWeaponClass_ = tokens[index + 2];
                    index += 2;
                } else if (command == "setfromlocstring" && index + 2 < tokens.size()) {
                    recognized = true;
                    const std::string dvar = LowerAscii(tokens[index + 1]);
                    if (dvar.rfind("customclass", 0) == 0) {
                        int32_t slot = 0;
                        if (ParseRetailInteger(dvar.substr(10), &slot)
                            && slot >= 1 && slot <= 5)
                            customClassNames_[slot - 1] = tokens[index + 2];
                    }
                    index += 2;
                } else if (command == "cmd") {
                    unsafe = true;
                } else if (command == "statgetindvar" && index + 2 < tokens.size()) {
                    recognized = true;
                    int32_t statIndex = 0;
                    if (ParseRetailInteger(tokens[index + 1], &statIndex)) {
                        int32_t statValue = 0;
                        if (statIndex >= 200 && statIndex < 250) {
                            const int32_t classSlot = (statIndex - 200) / 10;
                            const int32_t statOffset = (statIndex - 200) % 10;
                            if (classSlot >= 0 && classSlot < 5)
                                statValue = CustomClassStat(
                                    static_cast<uint32_t>(classSlot),
                                    static_cast<uint32_t>(statOffset));
                        }
                        dvarValues[LowerAscii(tokens[index + 2])] =
                            std::to_string(statValue);
                    }
                    index += 2;
                } else if (command == "execnowondvarstringvalue"
                           && index + 3 < tokens.size()) {
                    recognized = true;
                    const std::string dvar = LowerAscii(tokens[index + 1]);
                    const auto value = dvarValues.find(dvar);
                    if (value != dvarValues.end()
                        && LowerAscii(value->second) == LowerAscii(tokens[index + 2]))
                        execute(tokens[index + 3]);
                    index += 3;
                } else if (command == "setdvar" && index + 2 < tokens.size()) {
                    recognized = true;
                    dvarValues[LowerAscii(tokens[index + 1])] = tokens[index + 2];
                    index += 2;
                } else if (command == "statclearpitmask"
                           || command == "statclearperknew"
                           || command == "updategamerprofile") {
                    recognized = true;
                } else if (LowerAscii(tokens[index]).find("statset ") == 0
                           || LowerAscii(tokens[index]).find("set ") == 0) {
                    // Several retail execNowOnDvarStringValue branches carry
                    // their command sequence in one quoted argument.
                    execute(tokens[index]);
                }
            }
        }
    };
    execute(script);
    if (!recognized || unsafe)
        return false;

    if (!lastOpen.empty()) {
        if (lastOpen == "pc_cac_popup_ingame") {
            activeMenuName_ = lastOpen;
            classMenuHistory_.clear();
            screen_ = VitaFrontendScreen::CustomClasses;
            selection_ = 0;
            return true;
        }
        if (!menus_ || !menus_->Find(lastOpen.c_str()))
            return false;
        const std::string previousMenu = activeMenuName_;
        classMenuHistory_.push_back(previousMenu);
        activeMenuName_ = lastOpen;
        if (const int slot = CustomClassSlotForEditorMenu(lastOpen); slot >= 0) {
            activeCustomClassSlot_ = slot;
            definition = CustomClassForSlot(slot);
            classSelectionTargetStat_ = 0;
            screen_ = VitaFrontendScreen::ClassEditor;
        } else {
            classSelectionTargetStat_ =
                ClassTargetStatForMenu(lastOpen, definition);
            screen_ = VitaFrontendScreen::ClassPicker;
        }
        selection_ = 0;
        return true;
    }

    if (closeSelf || namedClose) {
        if (const CustomClassDefinition* editor =
                CustomClassForEditorMenu(activeMenuName_)) {
            (void)editor;
            activeMenuName_ = "pc_cac_popup_ingame";
            classMenuHistory_.clear();
            screen_ = VitaFrontendScreen::CustomClasses;
            selection_ = 0;
            return true;
        }
        for (auto history = classMenuHistory_.rbegin();
             history != classMenuHistory_.rend(); ++history) {
            if (CustomClassForEditorMenu(*history)) {
                activeMenuName_ = *history;
                classMenuHistory_.clear();
                screen_ = VitaFrontendScreen::ClassEditor;
                selection_ = 0;
                return true;
            }
        }
        Back();
        return true;
    }
    return true;
}

bool VitaFrontend::ExecuteClassMenuSelection() {
    const VitaMenuRecord* menu = ActiveClassMenu();
    const std::vector<int> indices = ClassMenuItemIndices();
    if (!menu || selection_ < 0 || selection_ >= static_cast<int>(indices.size()))
        return false;
    const VitaMenuItem& item = menu->items[indices[selection_]];
    const std::string label = item.text;
    if (screen_ == VitaFrontendScreen::ClassEditor) {
        const CustomClassDefinition* definition =
            CustomClassForSlot(activeCustomClassSlot_);
        if (definition) {
            if (label == "@MPUI_PRIMARY_WEAPON1")
                classSelectionTargetStat_ = definition->baseStat + 1;
            else if (label == "@MPUI_SIDE_ARM1"
                     || label == "@MPUI_SECONDARY_WEAPON")
                classSelectionTargetStat_ = definition->baseStat + 3;
            else if (label == "@MPUI_SPECIAL_GRENADE")
                classSelectionTargetStat_ = definition->baseStat + 8;
            else if (label == "@MPUI_PERK_1")
                classSelectionTargetStat_ = definition->baseStat + 5;
            else if (label == "@MPUI_PERK_2")
                classSelectionTargetStat_ = definition->baseStat + 6;
            else if (label == "@MPUI_PERK_3")
                classSelectionTargetStat_ = definition->baseStat + 7;
        }
    }
    return ExecuteClassMenuScript(RuntimeItemScript(item));
}

int VitaFrontend::WrapSelection(int selection, int count) {
    if (count <= 0)
        return 0;
    while (selection < 0)
        selection += count;
    while (selection >= count)
        selection -= count;
    return selection;
}

void VitaFrontend::Reset() {
    comparisonRequested_=false;
    shipmentTestRequested_=false;
    sourceReplayRequested_=false;
    cpuDiagnosticRequested_=0;
    screen_ = VitaFrontendScreen::Main;
    optionsReturnScreen_ = VitaFrontendScreen::Main;
    mapReturnScreen_ = VitaFrontendScreen::Multiplayer;
    gameMode_ = VitaGameMode::Deathmatch;
    selection_ = 1;
    loadingFrames_ = 0;
    selectedMapIndex_ = 0;
    availableMapMask_ = 0;
    loadingReady_ = false;
    SetLoadingStage("", 0, 0);
    sensitivityLevel_ = 1;
    invertY_ = false;
    rearTouchEnabled_ = true;
    dpadFallbackProfile_ = false;
    ResetCustomClassStats();
    activeCustomClassSlot_ = 0;
    classSelectionTargetStat_ = 0;
    activeMenuName_.clear();
    classMenuHistory_.clear();
    selectedWeaponClass_.clear();
    fatalMessage_.clear();
}

void VitaFrontend::SetDiagnosticScreen(VitaFrontendScreen screen) {
    screen_ = screen;
    optionsReturnScreen_ = VitaFrontendScreen::Main;
    mapReturnScreen_ = VitaFrontendScreen::Multiplayer;
    // Keep the initial highlight on the same first enabled item the retail
    // route would expose. Main is the one exception: Multiplayer is the first
    // enabled row after the disabled Single Player entry.
    selection_ = screen == VitaFrontendScreen::Main ? 1 : 0;
    if (screen == VitaFrontendScreen::CustomClasses) {
        activeMenuName_ = "pc_cac_popup_ingame";
        classMenuHistory_.clear();
    } else if (screen == VitaFrontendScreen::ClassEditor) {
        activeCustomClassSlot_ = 0;
        activeMenuName_ = "menu_cac_assault";
        classMenuHistory_.clear();
    } else if (screen == VitaFrontendScreen::ClassPicker) {
        activeMenuName_ = "assault_popup_cac_primary";
        classMenuHistory_.clear();
        classMenuHistory_.push_back("menu_cac_assault");
        classSelectionTargetStat_ = 201;
    }
    if (screen == VitaFrontendScreen::GameModeSelect)
        gameMode_ = VitaGameMode::Deathmatch;
    loadingFrames_ = screen == VitaFrontendScreen::Loading ? 45 : 0;
    loadingReady_ = screen != VitaFrontendScreen::Loading;
    SetLoadingStage("", 0, 0);
}

void VitaFrontend::SetSelectedMapIndex(uint32_t mapIndex) {
    const bool available = mapIndex < kisak::vita::kVitaMpMapCount
        && mapIndex < 32u
        && (availableMapMask_ & (1u << mapIndex)) != 0;
    selectedMapIndex_ = available ? mapIndex
        : static_cast<uint32_t>(FirstSelectable(
              VitaFrontendScreen::MapSelect, availableMapMask_));
    if (screen_ == VitaFrontendScreen::MapSelect)
        selection_ = static_cast<int>(selectedMapIndex_);
}

void VitaFrontend::SetMapAvailabilityError(uint32_t mapIndex,const char* reason) {
    if(mapIndex<kVitaMpMapCount)mapAvailabilityErrors_[mapIndex]=reason ? reason : "Content unavailable";
}

void VitaFrontend::SetAvailableMapMask(uint32_t mapMask) {
    availableMapMask_ = mapMask & kisak::vita::kVitaCertifiedMapMask;
    if (selectedMapIndex_ >= 32u
        || (availableMapMask_ & (1u << selectedMapIndex_)) == 0) {
        selectedMapIndex_ = static_cast<uint32_t>(FirstSelectable(
            VitaFrontendScreen::MapSelect, availableMapMask_));
    }
    if (screen_ == VitaFrontendScreen::MapSelect)
        selection_ = static_cast<int>(selectedMapIndex_);
}

void VitaFrontend::OpenPause() {
    if (screen_ != VitaFrontendScreen::InGame)
        return;
    screen_ = VitaFrontendScreen::Pause;
    selection_ = 0;
}

void VitaFrontend::Update() {
    if (screen_ != VitaFrontendScreen::Loading)
        return;
    if (loadingFrames_ > 0)
        --loadingFrames_;
    if (loadingFrames_ == 0 && loadingReady_)
        screen_ = VitaFrontendScreen::InGame;
}

void VitaFrontend::CompleteMapLoad() {
    if (screen_ != VitaFrontendScreen::Loading)
        return;
    loadingReady_ = true;
    // Let at least one frame display the authored load image/progress state
    // after the package closure is staged before entering gameplay.
    loadingFrames_ = std::min(loadingFrames_, 1);
}

void VitaFrontend::FailMapLoad(const char* message) {
    screen_ = VitaFrontendScreen::Fatal;
    loadingFrames_ = 0;
    loadingReady_ = false;
    SetLoadingStage("", 0, 0);
    selection_ = 0;
    fatalMessage_ = message && *message ? message : "CONTENT COULD NOT BE LOADED";
    if (fatalMessage_.size() > 120u)
        fatalMessage_.resize(120u);
}

const char* VitaFrontend::SelectedMapName() const {
    return selectedMapIndex_ < kisak::vita::kVitaMpMapCount
        ? kisak::vita::kVitaMpMaps[selectedMapIndex_].name : "shipment";
}

const char* VitaFrontend::SelectedMapDisplayName() const {
    return selectedMapIndex_ < kisak::vita::kVitaMpMapCount
        ? kisak::vita::kVitaMpMaps[selectedMapIndex_].displayName : "SHIPMENT";
}

float VitaFrontend::LoadingProgress() const {
    if (loadingReady_) return 1.0f;
    return loadingTotal_ ? std::min(1.0f, static_cast<float>(loadingCompleted_) / loadingTotal_) : 0.0f;
}

void VitaFrontend::SetLoadingStage(const char* stage, uint32_t completed, uint32_t total) {
    std::snprintf(loadingStage_, sizeof(loadingStage_), "%s", stage ? stage : "");
    loadingCompleted_ = std::min(completed, total);
    loadingTotal_ = total;
}

void VitaFrontend::Back() {
    switch (screen_) {
    case VitaFrontendScreen::Multiplayer:
        screen_ = VitaFrontendScreen::Main;
        selection_ = 1;
        break;
    case VitaFrontendScreen::GameModeSelect:
        screen_ = VitaFrontendScreen::Multiplayer;
        selection_ = 0;
        break;
    case VitaFrontendScreen::CustomClasses:
        screen_ = VitaFrontendScreen::Multiplayer;
        selection_ = 1;
        activeMenuName_.clear();
        classMenuHistory_.clear();
        break;
    case VitaFrontendScreen::ClassEditor:
        screen_ = VitaFrontendScreen::CustomClasses;
        activeMenuName_ = "pc_cac_popup_ingame";
        classMenuHistory_.clear();
        selection_ = 0;
        break;
    case VitaFrontendScreen::ClassPicker:
        if (!classMenuHistory_.empty()) {
            activeMenuName_ = classMenuHistory_.back();
            classMenuHistory_.pop_back();
            if (CustomClassForEditorMenu(activeMenuName_)) {
                screen_ = VitaFrontendScreen::ClassEditor;
                classSelectionTargetStat_ = 0;
            } else {
                classSelectionTargetStat_ = ClassTargetStatForMenu(
                    activeMenuName_, CustomClassForSlot(activeCustomClassSlot_));
                screen_ = VitaFrontendScreen::ClassPicker;
            }
            selection_ = 0;
        } else {
            screen_ = VitaFrontendScreen::ClassEditor;
            activeMenuName_ = CustomClassForSlot(activeCustomClassSlot_)->editorMenu;
            classSelectionTargetStat_ = 0;
            selection_ = 0;
        }
        break;
    case VitaFrontendScreen::Options:
        if (optionsReturnScreen_ == VitaFrontendScreen::Multiplayer) {
            screen_ = VitaFrontendScreen::Multiplayer;
            selection_ = 4;
        } else if (optionsReturnScreen_ == VitaFrontendScreen::Pause) {
            screen_ = VitaFrontendScreen::Pause;
            selection_ = 1;
        } else {
            screen_ = VitaFrontendScreen::Main;
            selection_ = 2;
        }
        break;
    case VitaFrontendScreen::Graphics:
        screen_ = VitaFrontendScreen::Options; selection_=1; break;
    case VitaFrontendScreen::Tests:
        screen_ = VitaFrontendScreen::Main; selection_=3; break;
    case VitaFrontendScreen::Controls:
        screen_ = VitaFrontendScreen::Options;
        selection_ = 0;
        break;
    case VitaFrontendScreen::ControlsLook:
        screen_ = VitaFrontendScreen::Controls;
        selection_ = 0;
        break;
    case VitaFrontendScreen::Pause:
        screen_ = VitaFrontendScreen::InGame;
        selection_ = 0;
        break;
    case VitaFrontendScreen::MapSelect:
        screen_ = mapReturnScreen_;
        selection_ = 0;
        break;
    default:
        break;
    }
}

bool VitaFrontend::HandleInput(const VitaInputSample& input, bool canStartMap) {
    // Loading advances only through readiness, never hidden accept/back rows.
    if (screen_ == VitaFrontendScreen::Loading) return false;
    if (screen_ == VitaFrontendScreen::Fatal) {
        if (!input.menuAcceptPressed && !input.menuBackPressed)
            return false;
        screen_ = VitaFrontendScreen::Main;
        selection_ = 1;
        fatalMessage_.clear();
        return true;
    }
    if (IsClassScreen()) {
        const std::vector<int> indices = ClassMenuItemIndices();
        const int count = static_cast<int>(indices.size());
        bool changed = false;
        if (count > 0 && (input.menuUpPressed || input.menuDownPressed)) {
            const int direction = input.menuDownPressed ? 1 : -1;
            selection_ = WrapSelection(selection_ + direction, count);
            changed = true;
        }
        if (input.menuBackPressed) {
            const VitaFrontendScreen before = screen_;
            Back();
            return changed || before != screen_;
        }
        if (input.menuAcceptPressed)
            return ExecuteClassMenuSelection() || changed;
        return changed;
    }
    const bool moved = input.menuUpPressed || input.menuDownPressed;
    bool focusChanged = false;
    if (moved) {
        const int previousSelection = selection_;
        const int direction = input.menuDownPressed ? 1 : -1;
        int count = 1;
        RouteItems(screen_, &count);
        int candidate = selection_;
        for (int attempt = 0; attempt < count; ++attempt) {
            candidate = WrapSelection(candidate + direction, count);
            if (IsSelectable(screen_, candidate, availableMapMask_)
                || (screen_==VitaFrontendScreen::MapSelect && candidate>=0
                    && VitaMapIsEnabled(static_cast<uint32_t>(candidate)))) {
                selection_ = candidate;
                break;
            }
        }
        focusChanged = selection_ != previousSelection;
        if (screen_ == VitaFrontendScreen::MapSelect
            && selection_ >= 0
            && selection_ < static_cast<int>(kisak::vita::kVitaMpMapCount))
            selectedMapIndex_ = static_cast<uint32_t>(selection_);
    }
    // ui_shared routes slider/listbox changes through LEFTARROW/RIGHTARROW,
    // independently of the activation key.  Keep that desktop contract for
    // the recovered options_look sensitivity row instead of requiring a
    // second Cross press (which previously made the setting appear inert).
    if (screen_ == VitaFrontendScreen::ControlsLook
        && (input.menuLeftPressed || input.menuRightPressed)) {
        int count = 0;
        const VitaFrontendRouteItem* items = RouteItems(screen_, &count);
        if (items && selection_ >= 0 && selection_ < count
            && items[selection_].enabled
            && items[selection_].action == VitaFrontendAction::AdjustSensitivity) {
            sensitivityLevel_ = std::clamp(
                sensitivityLevel_ + (input.menuRightPressed ? 1 : -1), 1, 3);
            return true;
        }
    }
    if (input.menuBackPressed) {
        const VitaFrontendScreen before = screen_;
        Back();
        return before != screen_;
    }
    if (screen_ == VitaFrontendScreen::Pause && input.startPressed) {
        Back();
        return true;
    }
    if (screen_==VitaFrontendScreen::Graphics && (input.menuLeftPressed || input.menuRightPressed || input.menuAcceptPressed)) {
        if(selection_==0) {
            const int delta=input.menuLeftPressed ? 2 : 1;
            renderSettings_.resolution=static_cast<VitaWorldResolution>((int(renderSettings_.resolution)+delta)%3);
            return true;
        }
        if(selection_==1) { renderSettings_.balancedGrass=!renderSettings_.balancedGrass; return true; }
#if KISAK_VITA_CPU_RUNTIME
        if(selection_==2) { renderSettings_.simpleWorldLighting=!renderSettings_.simpleWorldLighting; return true; }
#endif
    }
    if (screen_ == VitaFrontendScreen::Options) {
        if (selection_==1 && input.menuAcceptPressed) {
            screen_=VitaFrontendScreen::Graphics; selection_=0; return true;
        }
        if (input.menuAcceptPressed && ExecuteRetailMenuAction(selection_))
            return true;
        // The displayed top-level PC options are capability-gated.  Do not
        // let an accept press mutate a hidden Vita-only setting while the row
        // is visibly disabled.
        if (input.menuAcceptPressed
            && !IsSelectable(screen_, selection_, availableMapMask_))
            return false;
        return focusChanged;
    }
    if (!input.menuAcceptPressed)
        return focusChanged;

    int count = 0;
    const VitaFrontendRouteItem* items = RouteItems(screen_, &count);
    if (!items || selection_ < 0 || selection_ >= count)
        return focusChanged;
    if (!IsSelectable(screen_, selection_, availableMapMask_))
        return false;
    if (ExecuteRetailMenuAction(selection_))
        return true;
    switch (items[selection_].action) {
    case VitaFrontendAction::OpenMultiplayer:
            screen_ = VitaFrontendScreen::Multiplayer;
            selection_ = 0;
        return true;
    case VitaFrontendAction::OpenGameModeSelect:
            screen_ = VitaFrontendScreen::GameModeSelect;
            selection_ = 0;
        return true;
    case VitaFrontendAction::OpenOptions:
            optionsReturnScreen_ = VitaFrontendScreen::Main;
            if (screen_ == VitaFrontendScreen::Multiplayer)
                optionsReturnScreen_ = VitaFrontendScreen::Multiplayer;
            else if (screen_ == VitaFrontendScreen::Pause)
                optionsReturnScreen_ = VitaFrontendScreen::Pause;
            screen_ = VitaFrontendScreen::Options;
            selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    case VitaFrontendAction::RunComparison:
        comparisonRequested_=true;
        screen_=VitaFrontendScreen::Main; selection_=1;
        return true;
    case VitaFrontendAction::OpenTests:
        screen_=VitaFrontendScreen::Tests;selection_=FirstSelectable(screen_,availableMapMask_);return true;
    case VitaFrontendAction::RunShipmentTest:
        shipmentTestRequested_=true;
        screen_=VitaFrontendScreen::Main;selection_=3;return true;
    case VitaFrontendAction::RunCpuDiagnostic:
    case VitaFrontendAction::RunGpuTest:
    case VitaFrontendAction::RunGraphicsProfile:
        cpuDiagnosticRequested_=items[selection_].action==VitaFrontendAction::RunCpuDiagnostic
            ?1u:items[selection_].action==VitaFrontendAction::RunGraphicsProfile?35u:kCurrentGpuDiagnostic;
        screen_=VitaFrontendScreen::Main;selection_=3;
        return true;
    case VitaFrontendAction::OpenGraphics:
        screen_=VitaFrontendScreen::Graphics; selection_=0; return true;
    case VitaFrontendAction::OpenControls:
        screen_ = VitaFrontendScreen::Controls;
        selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    case VitaFrontendAction::OpenControlsLook:
        screen_ = VitaFrontendScreen::ControlsLook;
        selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    case VitaFrontendAction::ResumeGame:
        screen_ = VitaFrontendScreen::InGame;
        selection_ = 0;
        return true;
    case VitaFrontendAction::ExitToMain:
        screen_ = VitaFrontendScreen::Main;
        selection_ = 1;
        return true;
    case VitaFrontendAction::OpenMapSelect:
            mapReturnScreen_ = screen_;
            screen_ = VitaFrontendScreen::MapSelect;
            selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    case VitaFrontendAction::SelectDeathmatch:
        // The only enabled mode row is the authored `dm` ruleset. Keep this
        // state explicit so the HUD timer/score path can reject unsupported
        // objective modes instead of silently drawing a mismatched overlay.
        gameMode_ = VitaGameMode::Deathmatch;
        mapReturnScreen_ = VitaFrontendScreen::GameModeSelect;
        screen_ = VitaFrontendScreen::MapSelect;
        selection_ = FirstSelectable(screen_, availableMapMask_);
        return true;
    case VitaFrontendAction::StartShipment:
        if (canStartMap && IsDeathmatch()) {
            selectedMapIndex_ = static_cast<uint32_t>(
                std::clamp(selection_, 0, static_cast<int>(kisak::vita::kVitaMpMapCount - 1)));
            screen_ = VitaFrontendScreen::Loading;
            loadingFrames_ = 45;
            loadingReady_ = false;
    SetLoadingStage("", 0, 0);
        }
        return true;
    case VitaFrontendAction::Back:
        Back();
        return true;
    case VitaFrontendAction::AdjustSensitivity:
        // The authored PC slider is changed with left/right.  Accepting the
        // row only focuses it; do not mutate the value on Cross.
        return true;
    case VitaFrontendAction::ToggleInvertY:
        invertY_ = !invertY_;
        return true;
    case VitaFrontendAction::ToggleRearTouch:
        rearTouchEnabled_ = !rearTouchEnabled_;
        return true;
    case VitaFrontendAction::ToggleDpadFallback:
        dpadFallbackProfile_ = !dpadFallbackProfile_;
        return true;
    case VitaFrontendAction::ResetControls:
        sensitivityLevel_ = 1;
        invertY_ = false;
        rearTouchEnabled_ = true;
        dpadFallbackProfile_ = false;
        return true;
    case VitaFrontendAction::Disabled:
    default:
        // Disabled COD4 rows remain visible for route fidelity but must not
        // consume an accept press or emit the enabled-row UI click sound.
        return false;
    }
}

void VitaFrontend::ApplyInputConfig(VitaInputConfig* config) const {
    if (!config)
        return;
    config->lookSensitivity = 0.75f + 0.25f * static_cast<float>(sensitivityLevel_);
    config->invertY = invertY_;
    config->rearTouchEnabled = rearTouchEnabled_;
    config->dpadFallbackProfile = dpadFallbackProfile_;
}

void VitaFrontend::DrawCpuLoadingStatus(const GfxBackendApi* gfx,const char* stage,uint64_t elapsedUs) const {
    if (!gfx) return;
    // The selected map art owns a separate lease while source/menu assets
    // are rebuilt. Never dereference map-local fonts or menu records here.
    const float color[4]={0.88f,0.92f,0.95f,1.0f};
    gfx->SetSurfaceMode(GfxSurfaceMode::Overlay);
    if (gfx->ApplyViewEffects) {
        GfxViewEffectState effects{};effects.exposure=1.0f;effects.gamma=0.8f;
        gfx->ApplyViewEffects(&effects);
    }
    if (loadingTexture_.value) {
        const float white[4]={1,1,1,1}, panel[4]={0.01f,0.015f,0.025f,0.86f};
        DrawTexturedRect(gfx,loadingTexture_,-1,-1,1,1,0.52f,white);
        DrawRect(gfx,-1,-1,1,-0.79f,0.25f,panel);
        char status[192]{};
        if (elapsedUs) std::snprintf(status,sizeof(status),"%s  |  %u SECONDS",
            stage && *stage ? stage : "PREPARING MATCH",unsigned(elapsedUs/1000000));
        else std::snprintf(status,sizeof(status),"%s",stage && *stage ? stage : "PREPARING MATCH");
        DrawText(gfx,status,-0.92f,-0.92f,0.28f,color,0.2f);
        return;
    }
    // Before the single loading image is read, only resident glyphs are safe.
    gfx->BindTexture(GfxTextureHandle{});
    DrawText(gfx,"LOADING",-0.86f,0.25f,0.55f,color,0.2f);
    DrawText(gfx,stage?stage:"PREPARING",-0.86f,0.02f,0.33f,color,0.2f);
    char elapsed[64];std::snprintf(elapsed,sizeof(elapsed),"ELAPSED %u SECONDS",unsigned(elapsedUs/1000000));
    DrawText(gfx,elapsed,-0.86f,-0.18f,0.33f,color,0.2f);
}

void VitaFrontend::Draw(const GfxBackendApi* gfx) const {
    if (!gfx || screen_ == VitaFrontendScreen::InGame)
        return;
    if (screen_ == VitaFrontendScreen::Loading) {
        DrawCpuLoadingStatus(gfx,loadingStage_,0);
        return;
    }
    if (screen_ == VitaFrontendScreen::Fatal) {
        // A rejected map may have no usable asset pointers or GPU textures.
        // Draw this route entirely from resident glyphs, without resolving a
        // retail menu/font or binding a texture destroyed by map teardown.
        gfx->SetSurfaceMode(GfxSurfaceMode::Overlay);
        if (gfx->ApplyViewEffects) {
            GfxViewEffectState effects{};effects.exposure=1;effects.gamma=0.8f;
            gfx->ApplyViewEffects(&effects);
        }
        const float panel[4]={0.012f,0.018f,0.035f,1};
        const float text[4]={0.9f,0.93f,1,1};
        DrawRect(gfx,-1,-1,1,1,0.4f,panel);
        DrawText(gfx,"UNABLE TO LOAD MAP",-0.88f,0.55f,0.45f,text,0.2f);
        size_t cursor=0;
        for (unsigned line=0;line<5 && cursor<fatalMessage_.size();++line) {
            size_t count=std::min(size_t(52),fatalMessage_.size()-cursor);
            if (cursor+count<fatalMessage_.size()) {
                const size_t space=fatalMessage_.rfind(' ',cursor+count);
                if (space!=std::string::npos && space>cursor) count=space-cursor;
            }
            const auto message=fatalMessage_.substr(cursor,count);
            DrawText(gfx,message.c_str(),-0.88f,0.27f-0.16f*line,0.28f,text,0.2f);
            cursor+=count;
            while(cursor<fatalMessage_.size() && fatalMessage_[cursor]==' ')++cursor;
        }
        DrawText(gfx,"X: RETURN TO MAIN",-0.88f,-0.70f,0.33f,text,0.2f);
        return;
    }
    // The emulator and accelerated Vita use the same authored VITM/VITF
    // presentation.  The scalar backend is a slower rasterizer, but it must
    // not silently switch to a different menu with diagnostic headers.
    const GfxTextureHandle drawMenuTexture = menuTexture_;
    const GfxTextureHandle drawMapThumbnailTexture = mapThumbnailTexture_;
    const GfxTextureHandle drawMenuButtonTexture = menuButtonTexture_;
    const GfxTextureHandle drawMenuButtonSelectedTexture = menuButtonSelectedTexture_;
    const GfxTextureHandle drawLoadingTexture = loadingTexture_;
    const GfxTextureHandle drawFontTexture = fontTexture_;
    const VitaFontRecord* drawFont = font_;
    const VitaFontBundle* drawFonts = fonts_;
    const VitaFontRecord* drawTitleFont = ResolveRetailFont(
        drawFonts, 0.5833f, drawFont);
    std::string titleStorage;
    const auto Title = [&](const char* key, const char* fallback) {
        return ResolveLocalizedTitle(localized_, key, fallback, &titleStorage);
    };
    float panel[] = {0.012f, 0.018f, 0.035f, 0.94f};
    const float title[] = {0.90f, 0.93f, 1.0f, 1.0f};
    const float accent[] = {0.20f, 0.65f, 1.0f, 1.0f};
    const VitaMenuRecord* menu = nullptr;
    if (menus_ && menus_->Loaded()) {
        // These are the real menuDef names emitted by the recovered COD4 MP
        // oracle.  The compatibility labels below remain small, but colors
        // and layout defaults now come from the actual ui/ui_mp payload.
        const char* menuName = screen_ == VitaFrontendScreen::Main ? "background_main"
            : screen_ == VitaFrontendScreen::Multiplayer ? "main_controls"
            : screen_ == VitaFrontendScreen::GameModeSelect ? "main_controls"
            : screen_ == VitaFrontendScreen::Options ? "main_options"
            : screen_ == VitaFrontendScreen::Controls ? "main_controls"
            : screen_ == VitaFrontendScreen::ControlsLook ? "options_look"
            : screen_ == VitaFrontendScreen::CustomClasses ? "pc_cac_popup_ingame"
            : (screen_ == VitaFrontendScreen::ClassEditor
               || screen_ == VitaFrontendScreen::ClassPicker)
                ? activeMenuName_.c_str()
            : screen_ == VitaFrontendScreen::MapSelect ? "changemap" : nullptr;
        if (!menuName)
            menu = nullptr;
        else
            menu = menus_->Find(menuName);
        if (!menu && screen_ == VitaFrontendScreen::Options)
            menu = menus_->Find("main_options");
        if (menu) {
            bool hasBack = false;
            for (int channel = 0; channel < 4; ++channel)
                hasBack = hasBack || menu->backColor[channel] != 0.0f;
            if (hasBack)
                std::memcpy(panel, menu->backColor, sizeof(panel));
        }
    }
    const bool hasLoadingBackground = screen_ == VitaFrontendScreen::Loading
        && drawLoadingTexture.value != 0;
    const bool hasMenuBackground = screen_ != VitaFrontendScreen::Loading
        && drawMenuTexture.value != 0;
    if (hasLoadingBackground) {
        const float imageColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
        DrawTexturedRect(gfx, drawLoadingTexture, -1.0f, -1.0f, 1.0f, 1.0f,
                         0.52f, imageColor);
    } else if (hasMenuBackground) {
        const float imageColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
        DrawTexturedRect(gfx, drawMenuTexture, -1.0f, -1.0f, 1.0f, 1.0f,
                         0.52f, imageColor);
    } else {
        DrawRect(gfx, -1.0f, -1.0f, 1.0f, 1.0f, 0.50f, panel);
    }
    switch (screen_) {
    case VitaFrontendScreen::Main: {
        DrawRetailMenuTitle(gfx, "CALL OF DUTY 4", drawTitleFont, drawFontTexture);
        int count = 0;
        const auto* items = RouteItems(screen_, &count);
        DrawMenuItems(gfx, items, count, selection_, menu, localized_,
                      drawMenuButtonTexture, drawMenuButtonSelectedTexture,
                      drawFonts, drawFont, drawFontTexture);
        break;
    }
    case VitaFrontendScreen::Multiplayer: {
        DrawRetailMenuTitle(gfx, Title("MENU_MULTIPLAYER", "MULTIPLAYER"),
                            drawTitleFont, drawFontTexture);
        int count = 0;
        const auto* items = RouteItems(screen_, &count);
        DrawMenuItems(gfx, items, count, selection_, menu, localized_,
                      drawMenuButtonTexture, drawMenuButtonSelectedTexture,
                      drawFonts, drawFont, drawFontTexture);
        break;
    }
    case VitaFrontendScreen::CustomClasses:
    case VitaFrontendScreen::ClassEditor:
    case VitaFrontendScreen::ClassPicker: {
        const bool classList = screen_ == VitaFrontendScreen::CustomClasses;
        const bool editor = screen_ == VitaFrontendScreen::ClassEditor;
        const char* titleKey = classList ? "MPUI_YOUR_CLASSES"
            : editor ? "MPUI_CREATE_A_CLASS_CAP" : nullptr;
        const char* titleFallback = classList ? "YOUR CLASSES"
            : editor ? "CREATE A CLASS" : "SELECT";
        if (!classList && !editor) {
            if (activeMenuName_.find("_popup_cac_primary") != std::string::npos
                || activeMenuName_.find("_popup_cac_secondary") != std::string::npos
                || activeMenuName_.find("_popup_cac_primary2") != std::string::npos) {
                titleFallback = "WEAPONS";
            } else if (activeMenuName_.find("attachment_popup_") != std::string::npos) {
                titleFallback = "ATTACHMENTS";
            } else if (activeMenuName_.find("popup_cac_camo") != std::string::npos) {
                titleFallback = "CAMOUFLAGE";
            } else if (activeMenuName_.find("popup_cac_perk") != std::string::npos
                       || activeMenuName_.find("popup_cac_extra") != std::string::npos) {
                titleFallback = "PERKS";
            }
        }
        DrawRetailMenuTitle(gfx, Title(titleKey, titleFallback),
                            drawTitleFont, drawFontTexture);

        const std::vector<int> indices = ClassMenuItemIndices();
        std::vector<std::string> labels;
        labels.reserve(indices.size());
        const auto Localize = [&](const std::string& source) {
            if (localized_ && !source.empty()) {
                if (const auto* entry = localized_->Find(source.c_str())) {
                    if (!entry->value.empty())
                        return entry->value;
                }
            }
            return source;
        };
        for (const int itemIndex : indices) {
            if (!menu || itemIndex < 0
                || itemIndex >= static_cast<int>(menu->items.size())) {
                labels.emplace_back();
                continue;
            }
            const VitaMenuItem& item = menu->items[itemIndex];
            std::string label;
            if (classList) {
                const std::string target = ScriptOpenTarget(RuntimeItemScript(item));
                const int slot = CustomClassSlotForEditorMenu(target);
                if (slot >= 0) {
                    const CustomClassDefinition* definition = CustomClassForSlot(slot);
                    label = Localize(customClassNames_[slot].empty()
                                         ? definition->slotKey
                                         : customClassNames_[slot]);
                }
            }
            if (label.empty())
                label = Localize(!item.text.empty() ? item.text : item.name);
            labels.push_back(std::move(label));
        }
        DrawAuthoredClassMenu(gfx, menu, indices, labels, selection_,
                              drawMenuButtonTexture,
                              drawMenuButtonSelectedTexture, drawFonts, drawFont,
                              drawFontTexture);
        break;
    }
    case VitaFrontendScreen::GameModeSelect: {
        DrawRetailMenuTitle(gfx, Title("MENU_CHOOSE_GAME_MODE_CAP", "CHOOSE GAME MODE"),
                            drawTitleFont, drawFontTexture);
        int count = 0;
        const auto* items = RouteItems(screen_, &count);
        DrawMenuItems(gfx, items, count, selection_, menu, localized_,
                      drawMenuButtonTexture, drawMenuButtonSelectedTexture,
                      drawFonts, drawFont, drawFontTexture);
        break;
    }
    case VitaFrontendScreen::MapSelect: {
        DrawRetailMenuTitle(gfx, Title("MENU_CHOOSE_MAP_CAP", "CHOOSE MAP"),
                            drawTitleFont, drawFontTexture);
        std::array<VitaFrontendRouteItem, std::size(kMapRoute)> items{};
        std::copy(std::begin(kMapRoute), std::end(kMapRoute), items.begin());
        for (uint32_t index = 0; index < kVitaMpMapCount; ++index) {
            items[index].enabled = items[index].enabled && index < 32u
                && (availableMapMask_ & (1u << index)) != 0;
        }
        DrawMenuItems(gfx, items.data(), static_cast<int>(items.size()),
                      selection_, menu, localized_,
                      drawMenuButtonTexture, drawMenuButtonSelectedTexture,
                      drawFonts, drawFont, drawFontTexture, true);
        if (drawMapThumbnailTexture.value != 0) {
            const float imageColor[] = {1.0f, 1.0f, 1.0f, 0.94f};
            // changemap's maplist item is authored at (-9,53,200,270) in
            // the centered 640x480 virtual layout.
            const float mapLeftPx = VitaMenuPlacement::ApplyX(-9.0f, 2);
            const float mapTopPx = VitaMenuPlacement::ApplyY(53.0f, 0);
            const float mapWidthPx = 200.0f * VitaMenuPlacement::kScaleX;
            const float mapHeightPx = 270.0f * VitaMenuPlacement::kScaleY;
            DrawTexturedRect(gfx, drawMapThumbnailTexture,
                             VitaMenuPlacement::NdcX(mapLeftPx),
                             VitaMenuPlacement::NdcY(mapTopPx + mapHeightPx),
                             VitaMenuPlacement::NdcX(mapLeftPx + mapWidthPx),
                             VitaMenuPlacement::NdcY(mapTopPx),
                             0.24f, imageColor);
            DrawText(gfx, SelectedMapDisplayName(),
                     VitaMenuPlacement::NdcX(mapLeftPx + 12.0f),
                     VitaMenuPlacement::NdcY(mapTopPx + mapHeightPx - 8.0f), 0.4583f,
                     title, 0.20f, ResolveRetailFont(drawFonts, 0.4583f, drawFont),
                     drawFontTexture);
        }
        if(selection_>=0 && selection_<int(kVitaMpMapCount)
            && !(availableMapMask_&(1u<<selection_))) {
            const auto& reason=mapAvailabilityErrors_[selection_];
            DrawText(gfx,reason.empty()?"MAP PACK NOT INSTALLED":reason.c_str(),
                -0.70f,-0.88f,0.27f,title,0.20f,drawFont,drawFontTexture);
        }
        break;
    }
    case VitaFrontendScreen::Options: {
        DrawRetailMenuTitle(gfx, Title("MENU_OPTIONS_UPPER_CASE", "OPTIONS"),
                            drawTitleFont, drawFontTexture);
        int count = 0;
        const auto* items = RouteItems(screen_, &count);
        DrawMenuItems(gfx, items, count, selection_, menu, localized_,
                      drawMenuButtonTexture, drawMenuButtonSelectedTexture,
                      drawFonts, drawFont, drawFontTexture);
        break;
    }
    case VitaFrontendScreen::Tests: {
        DrawRetailMenuTitle(gfx,"TESTS",drawTitleFont,drawFontTexture);
        DrawMenuItems(gfx,kTestsRoute,int(std::size(kTestsRoute)),selection_,nullptr,nullptr,
            drawMenuButtonTexture,drawMenuButtonSelectedTexture,drawFonts,drawFont,drawFontTexture);
        const char* help="3 MINUTES - AUTOMATIC MOVEMENT AND COMBAT";
#if KISAK_VITA_CPU_RUNTIME
        help=selection_==0?"30 SECONDS - CPU SIMULATION WITHOUT SCENE SUBMISSION":selection_==2?"180 SECONDS - WORLD / PROPS / WEAPON / POST":kCurrentGpuHelp;
#endif
        DrawText(gfx,help,-0.70f,-0.55f,0.27f,title,0.2f,drawFont,drawFontTexture);
        DrawText(gfx,"START CANCELS - COPY LOGS AFTER EACH TEST",-0.70f,-0.64f,0.27f,title,0.2f,drawFont,drawFontTexture);
        break;
    }
    case VitaFrontendScreen::Graphics: {
        DrawRetailMenuTitle(gfx,"GRAPHICS",drawTitleFont,drawFontTexture);
        auto items=std::array<VitaFrontendRouteItem,std::size(kGraphicsRoute)>{};
        std::copy(std::begin(kGraphicsRoute),std::end(kGraphicsRoute),items.begin());
        const char* resolutions[]={"WORLD RESOLUTION: NATIVE","WORLD RESOLUTION: BALANCED","WORLD RESOLUTION: PERFORMANCE"};
        items[0].fallbackLabel=resolutions[unsigned(renderSettings_.resolution)];
        items[1].fallbackLabel=renderSettings_.balancedGrass ? "GRASS DETAIL: BALANCED" : "GRASS DETAIL: FULL";
#if KISAK_VITA_CPU_RUNTIME
        items[2].fallbackLabel=renderSettings_.simpleWorldLighting ? "WORLD LIGHTING: SIMPLE (EXPERIMENTAL)" : "WORLD LIGHTING: ORIGINAL";
#endif
        DrawMenuItems(gfx,items.data(),int(items.size()),selection_,nullptr,nullptr,
            drawMenuButtonTexture,drawMenuButtonSelectedTexture,drawFonts,drawFont,drawFontTexture);
        DrawText(gfx,"SETTINGS APPLY AT NEXT MAP LOAD",-0.70f,-0.55f,0.32f,title,0.2f,drawFont,drawFontTexture);
        DrawText(gfx,"WEAPON, HANDS AND HUD REMAIN NATIVE",-0.70f,-0.64f,0.30f,title,0.2f,drawFont,drawFontTexture);
        break;
    }
    case VitaFrontendScreen::Controls: {
        DrawRetailMenuTitle(gfx, Title("MENU_CONTROLS", "CONTROLS"),
                            drawTitleFont, drawFontTexture);
        int count = 0;
        const auto* items = RouteItems(screen_, &count);
        DrawMenuItems(gfx, items, count, selection_, menu, localized_,
                      drawMenuButtonTexture, drawMenuButtonSelectedTexture,
                      drawFonts, drawFont, drawFontTexture);
        break;
    }
    case VitaFrontendScreen::ControlsLook: {
        DrawRetailMenuTitle(gfx, Title("MENU_LOOK", "LOOK"),
                            drawTitleFont, drawFontTexture);
        int count = 0;
        const auto* items = RouteItems(screen_, &count);
        DrawMenuItems(gfx, items, count, selection_, menu, localized_,
                      drawMenuButtonTexture, drawMenuButtonSelectedTexture,
                      drawFonts, drawFont, drawFontTexture);
        // The PC options_look menu renders these as live dvar/listbox
        // ownerdraws in the right half of each 300px row. Keep the authored
        // row geometry and show the Vita-backed values instead of leaving
        // blank controls beside the labels.
        const float valueColor[] = {0.90f, 0.93f, 1.0f, 1.0f};
        const auto DrawLookValue = [&](const char* rowKey, const char* valueKey,
                                       const char* fallback, const char* value) {
            const auto* row = FindRuntimeMenuItem(menu, rowKey);
            if (!row)
                return;
            const float valueX = VitaMenuPlacement::NdcX(
                VitaMenuPlacement::ApplyX(row->rect[0] + 198.0f, 2));
            const float valueY = VitaMenuPlacement::NdcY(
                VitaMenuPlacement::ApplyY(row->rect[1] + 1.0f, 0));
            const auto* valueFont = ResolveRetailFont(
                drawFonts, row->textScale, drawFont);
            const char* display = value;
            std::string localizedValue;
            if (localized_ && valueKey) {
                if (const auto* entry = localized_->Find(valueKey)) {
                    localizedValue = entry->value;
                    if (!localizedValue.empty())
                        display = localizedValue.c_str();
                }
            }
            if (!display || !*display)
                display = fallback;
            DrawText(gfx, display, valueX, valueY,
                     std::clamp(row->textScale, 0.30f, 0.55f), valueColor,
                     0.27f, valueFont, drawFontTexture);
        };
        DrawLookValue("MENU_INVERT_MOUSE", invertY_ ? "PLATFORM_YES" : "PLATFORM_NO",
                      invertY_ ? "Yes" : "No", invertY_ ? "Yes" : "No");
        const char* sensitivityKey = sensitivityLevel_ <= 1 ? "MENU_LOW"
            : (sensitivityLevel_ == 2 ? "MENU_MEDIUM" : "MENU_HIGH");
        const char* sensitivityFallback = sensitivityLevel_ <= 1 ? "Low"
            : (sensitivityLevel_ == 2 ? "Medium" : "High");
        DrawLookValue("MENU_MOUSE_SENSITIVITY", sensitivityKey,
                      sensitivityFallback, sensitivityFallback);
        break;
    }
    case VitaFrontendScreen::Pause: {
        DrawRetailMenuTitle(gfx, Title("MENU_PAUSED_CAP", "PAUSED"),
                            drawTitleFont, drawFontTexture);
        int count = 0;
        const auto* items = RouteItems(screen_, &count);
        DrawMenuItems(gfx, items, count, selection_, menu, localized_,
                      drawMenuButtonTexture, drawMenuButtonSelectedTexture,
                      drawFonts, drawFont, drawFontTexture);
        break;
    }
    case VitaFrontendScreen::Loading:
        DrawRetailMenuTitle(gfx, Title("MENU_LOADING_DOTS", "LOADING..."),
                            drawTitleFont, drawFontTexture);
        DrawText(gfx, SelectedMapDisplayName(), -0.68f, 0.02f, 0.46f,
                 title, 0.22f, ResolveRetailFont(drawFonts, 0.46f, drawFont),
                 drawFontTexture);
        DrawRect(gfx, -0.68f, -0.10f, -0.68f + 1.36f * LoadingProgress(),
                 0.02f, 0.28f, accent);
        if (loadingStage_[0])
            DrawText(gfx, loadingStage_, -0.68f, -0.17f, 0.28f, title, 0.22f,
                     ResolveRetailFont(drawFonts, 0.28f, drawFont), drawFontTexture);
        break;
    default:
        break;
    }
    // Console COD4 uses a small controller hint in the lower-left corner,
    // not the opaque desktop Select/Back/Start/Exit bar. Keep the
    // Cross/Circle tokens as geometry (the PC glyph bytes are not present in
    // the compact Vita font stream) and leave the menu itself fully
    // navigable through the D-pad/face buttons handled above.
    const float footerText[] = {0.86f, 0.88f, 0.93f, 0.96f};
    DrawFooterCross(gfx, -0.855f, -0.947f, 0.18f, footerText);
    DrawText(gfx, "SELECT", -0.823f, -0.922f,
             0.32f, footerText, 0.18f, drawFont, drawFontTexture);
    DrawFooterCircle(gfx, -0.625f, -0.947f, 0.18f, footerText);
    DrawText(gfx, "BACK", -0.593f, -0.922f,
             0.32f, footerText, 0.18f, drawFont, drawFontTexture);
}

} // namespace kisak::vita
