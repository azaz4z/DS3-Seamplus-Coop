#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <atomic>

#ifndef DS3SC_FEATURE_ALLY_OUTLINE
#define DS3SC_FEATURE_ALLY_OUTLINE 1
#endif
#ifndef DS3SC_FEATURE_ALLY_MARKERS
#define DS3SC_FEATURE_ALLY_MARKERS 0
#endif
#ifndef DS3SC_FEATURE_PLAYER_OUTLINE
#define DS3SC_FEATURE_PLAYER_OUTLINE 0
#endif

#if (defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE) || \
    (defined(DS3SC_FEATURE_PLAYER_OUTLINE) && DS3SC_FEATURE_PLAYER_OUTLINE)
#define DS3SC_FEATURE_OUTLINE_CAPTURE 1
#else
#define DS3SC_FEATURE_OUTLINE_CAPTURE 0
#endif

namespace ds3sc::render {

class D3D11HookManager final {
public:
    static D3D11HookManager& Instance() noexcept;

    bool Install() noexcept;
    void Uninstall() noexcept;
    [[nodiscard]] bool IsInstalled() const noexcept { return installed_.load(); }

private:
    D3D11HookManager() = default;

    std::atomic<bool> installed_ = false;
};

void SetDrawingAlly(bool drawing) noexcept;
bool IsDrawingAlly() noexcept;

} // namespace ds3sc::render

extern "C" {
__declspec(dllexport) extern volatile LONG ds3scOutlineShowMask;
__declspec(dllexport) extern volatile LONG ds3scOutlineEnable;
__declspec(dllexport) extern volatile LONG ds3scOutlineThicknessInt;
__declspec(dllexport) extern volatile LONG ds3scOutlineFillSilhouette;
__declspec(dllexport) extern volatile LONG ds3scOutlineVisible;
__declspec(dllexport) extern volatile LONG ds3scOutlineFallbackMarkers;
__declspec(dllexport) extern volatile LONG ds3scPlayerOutlineEnable;
__declspec(dllexport) extern volatile LONG ds3scDiamondMarkersEnable;
__declspec(dllexport) extern volatile LONG ds3scD3D11Hooked;
__declspec(dllexport) extern volatile LONG ds3scAllyDrawsCount;
__declspec(dllexport) extern volatile LONG ds3scAllyDispatchCalls;
__declspec(dllexport) extern volatile LONG ds3scDisableVsync;
}

