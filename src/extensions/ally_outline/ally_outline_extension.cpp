#include "ally_outline_extension.h"
#include "../../render/d3d11_hook.h"
#include "../../render/ally_outline.h"
#include <algorithm>
#include <cmath>

namespace ds3sc::extensions {

bool AllyOutlineExtension::Initialize() noexcept {
    char iniPath[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CreateAllyOutlineExtension), &hMod) && hMod) {
        GetModuleFileNameA(hMod, iniPath, sizeof(iniPath));
        char* lastSlash = std::strrchr(iniPath, '\\');
        if (lastSlash) {
            strcpy_s(lastSlash + 1, sizeof(iniPath) - (lastSlash + 1 - iniPath), "ds3sc_settings.ini");
        }
    }
    if (iniPath[0] == '\0') {
        if (GetFileAttributesA("SeamplusCoop\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath, "SeamplusCoop\\ds3sc_settings.ini");
        } else {
            strcpy_s(iniPath, "SeamlessCoop\\ds3sc_settings.ini");
        }
    }
    const int fallback = GetPrivateProfileIntA("OUTLINE", "show_fallback_markers", 1, iniPath);
    InterlockedExchange(&ds3scOutlineFallbackMarkers, fallback != 0 ? 1 : 0);
    // Load saved render choices before installing the first Present hook.
    // The menu only edits individual settings; opening it is not initialization.
    InterlockedExchange(&ds3scOutlineEnable,
        GetPrivateProfileIntA("OUTLINE", "show_ally_outline", 0, iniPath) != 0);
    InterlockedExchange(&ds3scOutlineFillSilhouette,
        GetPrivateProfileIntA("OUTLINE", "fill_silhouette", 1, iniPath) != 0);
    InterlockedExchange(&ds3scOutlineThicknessInt,
        std::clamp(static_cast<int>(GetPrivateProfileIntA("OUTLINE", "outline_thickness", 20, iniPath)), 10, 40));

    return render::D3D11HookManager::Instance().Install();
}

void AllyOutlineExtension::Shutdown() noexcept {
    render::D3D11HookManager::Instance().Uninstall();
}

std::shared_ptr<IExtension> CreateAllyOutlineExtension() noexcept {
    return std::make_shared<AllyOutlineExtension>();
}

} // namespace ds3sc::extensions

// C exports for external control / test scripts
extern "C" {
__declspec(dllexport) void ds3sc_toggle_ally_mask(int show) {
    InterlockedExchange(&ds3scOutlineShowMask, show != 0);
}

__declspec(dllexport) void ds3sc_toggle_ally_outline(int enable) {
    InterlockedExchange(&ds3scOutlineEnable, enable != 0);
}

__declspec(dllexport) void ds3sc_toggle_outline_visible(int visible) {
    InterlockedExchange(&ds3scOutlineVisible, visible != 0);
}

__declspec(dllexport) void ds3sc_toggle_fallback_markers(int show) {
    InterlockedExchange(&ds3scOutlineFallbackMarkers, show != 0);
}

__declspec(dllexport) void ds3sc_set_outline_thickness(float thickness) {
    if (std::isfinite(thickness))
        InterlockedExchange(&ds3scOutlineThicknessInt, static_cast<LONG>(std::clamp(thickness, 1.0f, 4.0f) * 10.0f));
}

__declspec(dllexport) int ds3sc_is_d3d11_hooked() {
    return ds3sc::render::D3D11HookManager::Instance().IsInstalled() ? 1 : 0;
}
}

