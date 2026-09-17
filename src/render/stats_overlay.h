#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>

namespace ds3sc::render {

class CombatStatsOverlay final {
public:
    static CombatStatsOverlay& Instance() noexcept;

    HRESULT Present(IDXGISwapChain* swapChain) noexcept;
    void Reset() noexcept;

private:
    CombatStatsOverlay() = default;
    HRESULT EnsureResources(ID3D11Device* device, UINT width, UINT height) noexcept;

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
};

} // namespace ds3sc::render
