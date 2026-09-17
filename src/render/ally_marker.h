#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>

namespace ds3sc::render {

struct AllyMarkerSettings {
    bool enabled = true;
    float markerColor[3] = { 0.871f, 0.847f, 0.791f }; // DS3 ash white
    float pulseSpeed = 0.035f;
};

class LiveAllyMarkers final {
public:
    static LiveAllyMarkers& Instance() noexcept;

    HRESULT Initialize(ID3D11Device* device, UINT width, UINT height) noexcept;
    void Reset() noexcept;
    HRESULT Present(IDXGISwapChain* swap, bool enabled) noexcept;

private:
    LiveAllyMarkers() = default;
    ~LiveAllyMarkers() = default;

    LiveAllyMarkers(const LiveAllyMarkers&) = delete;
    LiveAllyMarkers& operator=(const LiveAllyMarkers&) = delete;

    struct MarkerConstants {
        float markerPos[8][4];
        float markerColor[4];
        UINT markerCount;
        UINT hasLocalMask;
        float markerPadding[2];
    };

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    UINT width_{0};
    UINT height_{0};

    Microsoft::WRL::ComPtr<ID3D11VertexShader> markerVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> markerPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> markerConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> markerBlendState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> markerDepthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> markerRaster_;


    std::recursive_mutex renderMutex_;
    float pulseTimer_{0.0f};
};

} // namespace ds3sc::render
