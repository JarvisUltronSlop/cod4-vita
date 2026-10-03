#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gfx/gfx_backend.h>
#include <vita/vita_input.h>
#include <vita/vita_map_assets.h>
#include <vita/vita_render_settings.h>
#include <vita/vita_menu_bundle.h>
#include <vita/vita_localize_bundle.h>
#include <vita/vita_stringtable_bundle.h>
#include <vita/vita_font_bundle.h>

namespace kisak::vita {

// This controller owns the offline vertical-slice menu state.  The retail MP
// boot route is script-driven and does not export a single `main` menuDef, so
// the source-controlled capability manifest keeps the COD4 route and disabled
// entries explicit while reusing the exported localization, menu colors, and
// layout records.  Unsupported PC menu categories stay visible/greyed rather
// than being replaced by Vita-only labels.
enum class VitaFrontendScreen : uint8_t {
    Main = 0,
    Multiplayer,
    CustomClasses,
    ClassEditor,
    ClassPicker,
    GameModeSelect,
    MapSelect,
    Options,
    Controls,
    ControlsLook,
    Loading,
    Pause,
    InGame,
    Fatal,
    Graphics,
    Tests,
};

// The vertical slice intentionally exposes only the retail `dm` ruleset.
// Keeping the unsupported retail modes in the route (disabled) lets the
// controller UI remain faithful while making the active mode explicit to the
// match/HUD bridge.
enum class VitaGameMode : uint8_t {
    Deathmatch = 0,
    TeamDeathmatch,
    Domination,
    Headquarters,
    SearchAndDestroy,
    Sabotage,
};

class VitaFrontend {
public:
    void Reset();
    void Update();

    // Test-only capture hook. Production input still starts at Main; a
    // diagnostic build may select another authored route so each menu can be
    // inspected without synthesizing controller edges in Vita3K.
    void SetDiagnosticScreen(VitaFrontendScreen screen);
    void SetSelectedMapIndex(uint32_t mapIndex);
    void SetAvailableMapMask(uint32_t mapMask);
    void SetMapAvailabilityError(uint32_t mapIndex, const char* reason);

    // Returns true when input changed the state. A transition into InGame is
    // allowed only when the selected map has a valid spawn/collision bridge.
    bool HandleInput(const VitaInputSample& input, bool canStartMap);
    // The bootstrap calls this after the selected map's package closure has
    // been staged and its local session is ready. Keeping this separate makes
    // the retail loading screen a real transition instead of a timed overlay.
    void CompleteMapLoad();
    // Enter a bounded local error screen after an OOM, corrupt pack, shader,
    // or map-stage failure. Retail content is never partially resumed; Cross
    // or Circle acknowledges the message and returns to Main.
    void FailMapLoad(const char* message);
    // Open the COD4 pause/options route without tearing down the local
    // Shipment session.  The bootstrap calls this on the gameplay Start edge.
    void OpenPause();
    void ApplyInputConfig(VitaInputConfig* config) const;
    void SetMenuBundle(const VitaMenuBundle* menus) { menus_ = menus; }
    void SetLocalizeBundle(const VitaLocalizeBundle* localized) { localized_ = localized; }
    void SetStringTableBundle(const VitaStringTableBundle* stringTables) {
        stringTables_ = stringTables;
    }
    void SetMenuBackground(GfxTextureHandle texture) { menuTexture_ = texture; }
    void SetMenuButtonTextures(GfxTextureHandle normal, GfxTextureHandle selected) {
        menuButtonTexture_ = normal;
        menuButtonSelectedTexture_ = selected;
    }
    void SetFont(const VitaFontRecord* font, GfxTextureHandle texture) {
        font_ = font;
        fontTexture_ = texture;
    }
    // Keep the complete recovered font table available so menu text can use
    // the same scale-dependent font selection as PC UI_GetFontHandle (small,
    // text, big, and extra-big records all share gamefonts_pc but have
    // different authored glyph metrics).
    void SetFontBundle(const VitaFontBundle* fonts, GfxTextureHandle texture) {
        fonts_ = fonts;
        fontTexture_ = texture;
        if (fonts_) {
            if (const VitaFontRecord* normal = fonts_->Find("fonts/normalFont"))
                font_ = normal;
        }
    }
    void SetLoadingBackground(GfxTextureHandle texture) { loadingTexture_ = texture; }
    void SetMapThumbnail(GfxTextureHandle texture) { mapThumbnailTexture_ = texture; }

    const VitaRenderSettings& RenderSettings() const { return renderSettings_; }
    bool ConsumeComparisonRequest() { const bool value=comparisonRequested_; comparisonRequested_=false; return value; }
    bool ConsumeShipmentTestRequest() {const bool value=shipmentTestRequested_;shipmentTestRequested_=false;return value;}
    bool ConsumeSourceReplayRequest() {const bool value=sourceReplayRequested_;sourceReplayRequested_=false;return value;}
    uint32_t ConsumeCpuDiagnosticRequest() {const auto request=cpuDiagnosticRequested_;cpuDiagnosticRequested_=0;return request;}
    VitaFrontendScreen Screen() const { return screen_; }
    bool InGame() const { return screen_ == VitaFrontendScreen::InGame; }
    VitaGameMode GameMode() const { return gameMode_; }
    bool IsDeathmatch() const { return gameMode_ == VitaGameMode::Deathmatch; }
    uint32_t SelectedMapIndex() const { return selectedMapIndex_; }
    const char* SelectedMapName() const;
    const char* SelectedMapDisplayName() const;
    float LoadingProgress() const;
    const char* LoadingStage() const { return loadingStage_; }
    void SetLoadingStage(const char* stage, uint32_t completed, uint32_t total);
    uint32_t ActiveCustomClassSlot() const {
        return static_cast<uint32_t>(activeCustomClassSlot_);
    }
    int32_t CustomClassStat(uint32_t slot, uint32_t offset) const;
    const char* CustomClassWeaponKey(uint32_t slot, bool secondary) const;

    // Draws the route manifest with exported VITM styling and VITZ labels.
    // Class edits stay in the frontend stat model until the gameplay bridge
    // consumes the selected loadout; the authored menu graph itself is fully
    // evaluated locally from the exported KisakCOD records.
    void Draw(const GfxBackendApi* gfx) const;
    void DrawCpuLoadingStatus(const GfxBackendApi* gfx,const char* stage,uint64_t elapsedUs) const;

private:
    static int WrapSelection(int selection, int count);
    void Back();
    bool ExecuteRetailMenuAction(int routeIndex);
    bool IsClassScreen() const;
    std::vector<int> ClassMenuItemIndices() const;
    bool ExecuteClassMenuSelection();
    bool ExecuteClassMenuScript(const std::string& script);
    void ResetCustomClassStats();
    void SetCustomClassStat(int32_t statIndex, int32_t value);
    const VitaMenuRecord* ActiveClassMenu() const;

    VitaFrontendScreen screen_ = VitaFrontendScreen::Main;
    VitaFrontendScreen optionsReturnScreen_ = VitaFrontendScreen::Main;
    VitaFrontendScreen mapReturnScreen_ = VitaFrontendScreen::Multiplayer;
    VitaGameMode gameMode_ = VitaGameMode::Deathmatch;
    int selection_ = 1; // Multiplayer is the first enabled Main entry.
    int loadingFrames_ = 0;
    uint32_t selectedMapIndex_ = 0;
    uint32_t availableMapMask_ = 0;
    std::array<std::string,kVitaMpMapCount> mapAvailabilityErrors_{};
    bool loadingReady_ = false;
    char loadingStage_[64]{};
    uint32_t loadingCompleted_ = 0, loadingTotal_ = 0;
    VitaRenderSettings renderSettings_{};
    bool comparisonRequested_=false;
    bool shipmentTestRequested_=false;
    bool sourceReplayRequested_=false;
    uint32_t cpuDiagnosticRequested_=0;
    int sensitivityLevel_ = 1;
    bool invertY_ = false;
    bool rearTouchEnabled_ = true;
    bool dpadFallbackProfile_ = false;
    const VitaMenuBundle* menus_ = nullptr;
    const VitaLocalizeBundle* localized_ = nullptr;
    const VitaStringTableBundle* stringTables_ = nullptr;
    const VitaFontBundle* fonts_ = nullptr;
    GfxTextureHandle menuTexture_{0};
    GfxTextureHandle menuButtonTexture_{0};
    GfxTextureHandle menuButtonSelectedTexture_{0};
    const VitaFontRecord* font_ = nullptr;
    GfxTextureHandle fontTexture_{0};
    GfxTextureHandle loadingTexture_{0};
    GfxTextureHandle mapThumbnailTexture_{0};
    std::string fatalMessage_;
    static constexpr size_t kCustomClassCount = 5;
    static constexpr size_t kClassStatCount = 10;
    std::array<std::array<int32_t, kClassStatCount>, kCustomClassCount>
        customClassStats_{};
    std::array<std::array<std::string, 2>, kCustomClassCount>
        customClassWeaponKeys_{};
    std::array<std::string, kCustomClassCount> customClassNames_{};
    int activeCustomClassSlot_ = 0;
    int classSelectionTargetStat_ = 0;
    std::string activeMenuName_;
    std::vector<std::string> classMenuHistory_;
    std::string selectedWeaponClass_;
};

} // namespace kisak::vita
