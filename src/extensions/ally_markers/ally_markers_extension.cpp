#include "ally_markers_extension.h"
#include "../../render/d3d11_hook.h"
#include "../../render/ally_marker.h"
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
extern volatile LONG ds3scDiamondMarkersEnable;
extern volatile LONG ds3scOutlineFallbackMarkers;
}

namespace ds3sc::extensions {

bool AllyMarkersExtension::Initialize() noexcept {
    char iniPath[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CreateAllyMarkersExtension), &hMod) && hMod) {
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

    // Read [ALLY_MARKERS] enabled with fallback to [OUTLINE] show_fallback_markers
    int enabled = GetPrivateProfileIntA("ALLY_MARKERS", "enabled", -1, iniPath);
    if (enabled == -1) {
        enabled = GetPrivateProfileIntA("OUTLINE", "show_fallback_markers", 1, iniPath);
    }
    const LONG enableVal = (enabled != 0) ? 1 : 0;
    InterlockedExchange(&ds3scDiamondMarkersEnable, enableVal);
    InterlockedExchange(&ds3scOutlineFallbackMarkers, enableVal);

    return render::D3D11HookManager::Instance().Install();
}

void AllyMarkersExtension::Shutdown() noexcept {
    render::D3D11HookManager::Instance().Uninstall();
}

std::shared_ptr<IExtension> CreateAllyMarkersExtension() noexcept {
    return std::make_shared<AllyMarkersExtension>();
}

} // namespace ds3sc::extensions

extern "C" {
__declspec(dllexport) void ds3sc_toggle_ally_markers(int show) {
    const LONG val = show != 0 ? 1 : 0;
    InterlockedExchange(&ds3scDiamondMarkersEnable, val);
    InterlockedExchange(&ds3scOutlineFallbackMarkers, val);
}

__declspec(dllexport) int ds3sc_are_ally_markers_enabled() {
    return ds3scDiamondMarkersEnable != 0 ? 1 : 0;
}
}
