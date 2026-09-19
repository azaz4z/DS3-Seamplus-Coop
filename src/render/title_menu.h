#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

namespace ds3sc::render {

struct MenuSettingItem {
    std::string name;
    std::string valueDisplay;
    int type; // 0 = bool, 1 = int range, 2 = info string, 3 = separator/header
    int valInt;
    int minInt;
    int maxInt;
    int stepInt;
    const char* section;
    const char* key;
    bool visible = true;
    int indent = 0;
    bool dirty = false;
};

class TitleMenu final {
public:
    static TitleMenu& Instance() noexcept;

    HRESULT Present(IDXGISwapChain* swapChain) noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool IsModalOpen() const noexcept { return isModalOpen_; }
    void SetModalOpen(bool open) noexcept;
    void RequestModalOpen() noexcept { openRequested_.store(true); }

    void LoadSettingsFromIni() noexcept;
    bool SaveSettingsToIni() noexcept;
    void EnsureSteamHook() noexcept;
    void EnsureInputHooks(HWND hWnd) noexcept;
    void ProcessGamepadInput(unsigned short wButtons, short thumbLX = 0, short thumbLY = 0) noexcept;
    [[nodiscard]] float GetTextWidth(const char* str, float scale = 1.0f) const noexcept;

private:
    friend struct TitleMenuTestAccess;
    TitleMenu();
    HRESULT EnsureResources(ID3D11Device* device, UINT width, UINT height) noexcept;
    void UpdateItemDisplays() noexcept;
    void RefreshLiveSettings() noexcept;
    void ApplyLiveSetting(const MenuSettingItem& item) noexcept;
    void AdjustItem(int index, bool forward) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> fontTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fontSRV_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> fontSampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterState_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

    UINT width_ = 0;
    UINT height_ = 0;

    std::atomic<bool> isModalOpen_{false};
    std::atomic<bool> openRequested_{false};
    bool settingsDirty_ = false;
    bool gamepadLeftHeld_ = false;
    bool gamepadRightHeld_ = false;
    bool gamepadInputReady_ = false;
    unsigned short gamepadButtonsHeld_ = 0;
    bool keyboardLeftHeld_ = false;
    bool keyboardRightHeld_ = false;
    int selectedItemIndex_ = -1;
    bool usingGamepadOrKeyboard_ = false;
    bool isTitleItemHovered_ = false;
    std::vector<MenuSettingItem> items_;
    std::wstring iniPath_;
    std::uint64_t lastInputTick_ = 0;
    std::uint64_t lastGamepadTick_ = 0;
    std::uint64_t modalOpenTick_ = 0;
    std::string saveStatusText_;
    std::uint64_t saveStatusTick_ = 0;
    int charWidths_[95] = {};
};

} // namespace ds3sc::render

extern "C" {
__declspec(dllexport) extern volatile LONG ds3scConnectionMode;
__declspec(dllexport) extern volatile LONG ds3scLanPort;
}
