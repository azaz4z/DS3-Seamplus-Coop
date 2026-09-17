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

namespace ds3sc::render {

struct MenuSettingItem {
    std::string name;
    std::string valueDisplay;
    int type; // 0 = bool, 1 = int range, 2 = info string
    int valInt;
    int minInt;
    int maxInt;
    int stepInt;
    const char* section;
    const char* key;
};

class TitleMenu final {
public:
    static TitleMenu& Instance() noexcept;

    HRESULT Present(IDXGISwapChain* swapChain) noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool IsModalOpen() const noexcept { return isModalOpen_; }
    void SetModalOpen(bool open) noexcept {
        isModalOpen_ = open;
        if (open) {
            modalOpenTick_ = GetTickCount64();
            lastInputTick_ = modalOpenTick_;
            lastGamepadTick_ = modalOpenTick_;
            selectedItemIndex_ = -1;
            usingGamepadOrKeyboard_ = false;
        }
    }

    void LoadSettingsFromIni() noexcept;
    void SaveSettingsToIni() noexcept;
    void EnsureSteamHook() noexcept;
    void EnsureInputHooks(HWND hWnd) noexcept;
    void ProcessGamepadInput(unsigned short wButtons) noexcept;
    [[nodiscard]] float GetTextWidth(const char* str, float scale = 1.0f) const noexcept;

private:
    TitleMenu();
    HRESULT EnsureResources(ID3D11Device* device, UINT width, UINT height) noexcept;
    void UpdateItemDisplays() noexcept;
    void ApplyLiveSettings() noexcept;

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

    bool isModalOpen_ = false;
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
