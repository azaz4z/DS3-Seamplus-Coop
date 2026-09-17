#define NOMINMAX
#include "../src/render/ally_outline.h"
#include <d3d11sdklayers.h>
#include <iostream>
#include <vector>
#include <stdexcept>

using namespace ds3sc::render;
using Microsoft::WRL::ComPtr;

void Check(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}

int main() {
    try {
        std::cout << "[TEST] Running AllyOutlineRenderer D3D11 WARP test..." << std::endl;

        D3D_FEATURE_LEVEL level{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_DEBUG,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device,
            &level,
            &context
        );
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
        }
        Check(SUCCEEDED(hr), "Failed to create WARP D3D11 device");

        auto& renderer = AllyOutlineRenderer::Instance();
        hr = renderer.EnsureResources(device.Get(), 1280, 720);
        Check(SUCCEEDED(hr), "EnsureResources failed");
        Check(renderer.IsInitialized(), "Renderer not initialized");
        Check(renderer.Width() == 1280, "Width mismatch");
        Check(renderer.Height() == 720, "Height mismatch");

        renderer.ClearMask(context.Get());

        // Test configuration
        renderer.Settings().showAllyOutline = true;
        renderer.Settings().thickness = 2.5f;
        renderer.Settings().opacity = 0.8f;
        Check(renderer.Settings().showAllyOutline == true, "Settings assignment failed");

        renderer.Reset();
        Check(!renderer.IsInitialized(), "Reset failed");

        std::cout << "[PASS] AllyOutlineRenderer initialized, created R8_UNORM mask, and cleaned up cleanly!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << std::endl;
        return 1;
    }
}
