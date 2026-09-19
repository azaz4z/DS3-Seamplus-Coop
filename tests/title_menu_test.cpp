#include "../src/render/title_menu.h"
#include "../src/render/d3d11_hook.h"
#include "../src/extensions/ally_outline/ally_outline_extension.h"
#include "../src/extensions/ally_markers/ally_markers_extension.h"
#include "../src/extensions/player_outline/player_outline_extension.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

// Link the real menu and extension initializers without installing game hooks.
extern "C" {
volatile LONG ds3scOutlineShowMask = 0;
volatile LONG ds3scOutlineEnable = 0;
volatile LONG ds3scOutlineThicknessInt = 20;
volatile LONG ds3scOutlineFillSilhouette = 1;
volatile LONG ds3scOutlineVisible = 0;
volatile LONG ds3scOutlineFallbackMarkers = 1;
volatile LONG ds3scPlayerOutlineEnable = 0;
volatile LONG ds3scDiamondMarkersEnable = 1;
volatile LONG ds3scDiamondMarkerHeightCm = 155;
volatile LONG ds3scDisableVsync = 0;
}

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

namespace ds3sc::render {
D3D11HookManager& D3D11HookManager::Instance() noexcept {
    static D3D11HookManager manager;
    return manager;
}
bool D3D11HookManager::Install() noexcept { return true; }
void D3D11HookManager::Uninstall() noexcept {}

struct TitleMenuTestAccess {
    static std::unique_ptr<TitleMenu> Create() { return std::unique_ptr<TitleMenu>(new TitleMenu); }
    static int Find(TitleMenu& menu, const char* section, const char* key) {
        for (std::size_t i = 0; i < menu.items_.size(); ++i) {
            if (!std::strcmp(menu.items_[i].section, section) && !std::strcmp(menu.items_[i].key, key))
                return static_cast<int>(i);
        }
        throw std::runtime_error("Required setting missing from menu");
    }
    static int Value(TitleMenu& menu, const char* section, const char* key) {
        return menu.items_[Find(menu, section, key)].valInt;
    }
    static void Change(TitleMenu& menu, const char* section, const char* key, bool forward = true) {
        menu.AdjustItem(Find(menu, section, key), forward);
    }
    static void Path(TitleMenu& menu, const std::filesystem::path& path) { menu.iniPath_ = path.wstring(); }
    static void Select(TitleMenu& menu, const char* section, const char* key) {
        menu.selectedItemIndex_ = Find(menu, section, key);
    }
    static void Elapse(TitleMenu& menu) { menu.lastGamepadTick_ = menu.modalOpenTick_ = 0; }
    static bool Dirty(TitleMenu& menu) { return menu.settingsDirty_; }
};
}

using Access = ds3sc::render::TitleMenuTestAccess;
using State = std::array<LONG, 8>;
State Snapshot() {
    return {ds3scDiamondMarkersEnable, ds3scOutlineFallbackMarkers, ds3scOutlineEnable,
            ds3scPlayerOutlineEnable, ds3scOutlineFillSilhouette, ds3scOutlineThicknessInt,
            ds3scConnectionMode, ds3scLanPort};
}

int main() try {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto ini = std::filesystem::path(executable).parent_path() / "ds3sc_settings.ini";
    {
        std::ofstream file(ini, std::ios::trunc);
        file << "[OUTLINE]\nshow_ally_outline=1\nfill_silhouette=1\noutline_thickness=30\n"
                "show_fallback_markers=0\noutline_local_player=0\n"
                "[ALLY_MARKERS]\nenabled=1\n[PLAYER_OUTLINE]\nenabled=1\n"
                "[NETWORK]\nconnection_mode=1\nlan_port=28123\n"
                "[GAMEPLAY]\nskip_intros=1\nallow_invaders=1\n";
        Check(static_cast<bool>(file), "Cannot write isolated test INI");
    }
    ds3sc::extensions::AllyOutlineExtension ally;
    ds3sc::extensions::PlayerOutlineExtension player;
    ds3sc::extensions::AllyMarkersExtension markers;
    Check(ally.Initialize() && player.Initialize() && markers.Initialize(), "Extension initialization failed");
    Check(ds3scOutlineEnable == 1 && ds3scOutlineFillSilhouette == 1 && ds3scOutlineThicknessInt == 30,
          "Saved ally outline/style did not become active before opening the menu");
    Check(ds3scPlayerOutlineEnable == 1 && ds3scDiamondMarkersEnable == 1 && ds3scOutlineFallbackMarkers == 1,
          "Explicit player/marker settings must take precedence over legacy keys");

    auto menu = Access::Create();
    const State startup = Snapshot();
    menu->SetModalOpen(true);
    Check(Snapshot() == startup, "Opening the menu changed active render settings");
    Check(Access::Value(*menu, "OUTLINE", "outline_thickness") == 30, "Thickness not loaded");
    menu->SetModalOpen(false);
    Check(GetPrivateProfileIntW(L"OUTLINE", L"show_fallback_markers", 9, ini.c_str()) == 0,
          "Opening and closing without edits rewrote the INI");
    menu->SetModalOpen(true);

    struct Toggle { const char* section; const char* key; int field; };
    for (const auto& item : {Toggle{"ALLY_MARKERS", "enabled", 0}, {"OUTLINE", "show_ally_outline", 2},
                            {"PLAYER_OUTLINE", "enabled", 3}, {"OUTLINE", "fill_silhouette", 4},
                            {"GAMEPLAY", "skip_intros", -1}, {"GAMEPLAY", "allow_invaders", -1}}) {
        auto expected = Snapshot();
        if (item.field >= 0) expected[item.field] = expected[item.field] ? 0 : 1;
        if (item.field == 0) expected[1] = expected[0];
        Access::Change(*menu, item.section, item.key);
        Check(Snapshot() == expected, "A toggle changed another active setting");
        menu->SetModalOpen(false); // Includes implicit Save on close.
        menu->SetModalOpen(true);
        Check(Snapshot() == expected, "Close/reopen changed active settings");
        if (item.field >= 0)
            Check(Access::Value(*menu, item.section, item.key) == expected[item.field], "Reopened menu shows stale value");
        Access::Change(*menu, item.section, item.key);
        Check(menu->SaveSettingsToIni(), "Save failed");
    }
    Access::Change(*menu, "OUTLINE", "outline_thickness");
    menu->SetModalOpen(false);
    menu->SetModalOpen(true);
    Check(ds3scOutlineThicknessInt == 40 && Access::Value(*menu, "OUTLINE", "outline_thickness") == 40,
          "Thickness reset to 20 after reopen");
    Access::Change(*menu, "OUTLINE", "outline_thickness", false);
    Check(menu->SaveSettingsToIni(), "Thickness save failed");

    // A hotkey/export can change live state without updating its INI key.
    ds3scOutlineFillSilhouette = 0;
    const State external = Snapshot();
    Access::Change(*menu, "GAMEPLAY", "skip_intros");
    Check(Snapshot() == external, "Unrelated menu edit overwrote a live render setting");
    Check(WritePrivateProfileStringW(L"OUTLINE", L"fill_silhouette", L"0", ini.c_str()), "External edit failed");
    Check(menu->SaveSettingsToIni(), "Unrelated save failed");
    Check(GetPrivateProfileIntW(L"OUTLINE", L"fill_silhouette", 9, ini.c_str()) == 0,
          "Save rewrote an untouched setting from an old UI snapshot");
    menu->SetModalOpen(false);
    menu->SetModalOpen(true);
    Check(Access::Value(*menu, "OUTLINE", "fill_silhouette") == 0, "UI not refreshed from live state");

    Access::Select(*menu, "ALLY_MARKERS", "enabled");
    Access::Elapse(*menu);
    menu->ProcessGamepadInput(0); // Establish neutral controller state.
    menu->ProcessGamepadInput(0x1000); // A.
    const LONG afterPress = ds3scDiamondMarkersEnable;
    Access::Elapse(*menu);
    menu->ProcessGamepadInput(0x1000); // Holding A beyond repeat delay.
    Check(ds3scDiamondMarkersEnable == afterPress, "Holding A toggled the same setting twice");
    menu->ProcessGamepadInput(0);
    menu->ProcessGamepadInput(0x1000); // Second press, even inside the repeat delay.
    Check(ds3scDiamondMarkersEnable != afterPress, "Second A press was lost");
    menu->SetModalOpen(true); // Duplicate native-open notification.
    Check(Access::Dirty(*menu), "Duplicate open discarded edits");

    Access::Path(*menu, ini.parent_path() / "missing-directory" / "settings.ini");
    menu->SetModalOpen(false);
    Check(menu->IsModalOpen() && Access::Dirty(*menu), "Failed save closed menu and discarded pending changes");
    Access::Path(*menu, ini);
    menu->SetModalOpen(false);
    Check(!menu->IsModalOpen(), "Could not close after retrying save");
    const State saved = Snapshot();
    ds3scOutlineEnable = ds3scPlayerOutlineEnable = ds3scDiamondMarkersEnable = 0;
    ds3scOutlineFillSilhouette = 1;
    Check(ally.Initialize() && player.Initialize() && markers.Initialize(), "Reload failed");
    Check(Snapshot() == saved, "Saved state did not survive extension reinitialization");
    std::cout << "PASS: real menu/INI round-trip, startup enabled flags, isolated toggles, live state, held input and save failure.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
