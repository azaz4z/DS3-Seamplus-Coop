#define NOMINMAX
#include "../src/render/ally_outline.h"
#include <d3d11sdklayers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using ds3sc::render::AllyOutline;
using ds3sc::render::OutlineFrame;
constexpr UINT W = 320, H = 180;
struct Pixel { float r, g, b, a; };
struct Mask { float coverage, depth; };
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void HR(HRESULT hr) { Check(SUCCEEDED(hr), "D3D11 operation failed"); }

struct Fixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> diagnostics;
    ComPtr<ID3D11Texture2D> maskTexture, depthTexture, target, readback;
    ComPtr<ID3D11ShaderResourceView> maskView, depthView;
    ComPtr<ID3D11RenderTargetView> targetView;
    AllyOutline outline;
    std::vector<Mask> mask{W * H};
    std::vector<float> depth = std::vector<float>(W * H, 0.4f);
    std::vector<Pixel> background{W * H};

    Fixture() {
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
            &device, &level, &context);
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device, &level, &context));
        } else HR(hr);
        device.As(&diagnostics);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = W; desc.Height = H; desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        HR(device->CreateTexture2D(&desc, nullptr, &maskTexture));
        HR(device->CreateShaderResourceView(maskTexture.Get(), nullptr, &maskView));
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        HR(device->CreateTexture2D(&desc, nullptr, &depthTexture));
        HR(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView));
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        HR(device->CreateTexture2D(&desc, nullptr, &target));
        HR(device->CreateRenderTargetView(target.Get(), nullptr, &targetView));
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&desc, nullptr, &readback));
        HR(outline.Initialize(device.Get()));
        for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x)
            background[y * W + x] = {0.028f, 0.025f, 0.022f, 0.37f};
        // A complete surrogate actor. Identity and mesh capture are deliberately
        // NOT simulated as a proven DS3 integration by this graphics fixture.
        for (UINT y = 35; y < 151; ++y) for (UINT x = 130; x < 191; ++x) {
            const bool head = (int(x) - 160) * (int(x) - 160) +
                (int(y) - 48) * (int(y) - 48) < 14 * 14;
            const bool body = y >= 62 && y < 110 && x >= 141 && x < 180;
            const bool arms = y >= 66 && y < 107 && (x < 141 || x >= 180);
            const bool legs = y >= 110 && ((x >= 142 && x < 155) || (x >= 165 && x < 178));
            if (head || body || arms || legs) mask[y * W + x] = {1, 0.7f};
        }
    }

    OutlineFrame Frame() {
        return {maskView.Get(), depthView.Get(), targetView.Get(), 7, 7, 7, true, true, false};
    }

    std::vector<Pixel> Draw(OutlineFrame frame, HRESULT expected = S_OK) {
        context->UpdateSubresource(maskTexture.Get(), 0, nullptr, mask.data(), W * sizeof(Mask), 0);
        context->UpdateSubresource(depthTexture.Get(), 0, nullptr, depth.data(), W * sizeof(float), 0);
        context->UpdateSubresource(target.Get(), 0, nullptr, background.data(), W * sizeof(Pixel), 0);
        // Deliberately bind state that conflicts with the outline pass. It must be
        // restored, including resources affected indirectly by hazard unbinding.
        ID3D11RenderTargetView* rtv = targetView.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srv = depthView.Get();
        context->PSSetShaderResources(5, 1, &srv);
        D3D11_VIEWPORT viewport{3, 5, 101, 87, 0.1f, 0.9f};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        Check(outline.Draw(context.Get(), frame) == expected, "unexpected render result");
        ComPtr<ID3D11RenderTargetView> restoredTarget;
        context->OMGetRenderTargets(1, &restoredTarget, nullptr);
        Check(restoredTarget == targetView, "render target state leaked");
        ComPtr<ID3D11ShaderResourceView> restoredSrv;
        context->PSGetShaderResources(5, 1, &restoredSrv);
        Check(restoredSrv == depthView, "shader resource state leaked");
        D3D11_VIEWPORT restoredViewport{};
        UINT count = 1;
        context->RSGetViewports(&count, &restoredViewport);
        Check(count == 1 && std::memcmp(&viewport, &restoredViewport, sizeof(viewport)) == 0,
              "viewport state leaked");
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        context->IAGetPrimitiveTopology(&topology);
        Check(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "topology state leaked");
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HR(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<Pixel> result(W * H);
        for (UINT y = 0; y < H; ++y)
            std::memcpy(result.data() + y * W,
                static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, W * sizeof(Pixel));
        context->Unmap(readback.Get(), 0);
        for (std::size_t i = 0; i < result.size(); ++i)
            Check(result[i].a == background[i].a, "destination alpha modified");
        return result;
    }

    void Unchanged(const std::vector<Pixel>& output) {
        Check(std::memcmp(output.data(), background.data(), output.size() * sizeof(Pixel)) == 0,
              "pixels changed when outline should be absent");
    }
};

void WritePreview(const std::vector<Pixel>& output, const char* path) {
    // Test artifact, not a DS3 screenshot. BMP top-down, RGB converted from linear.
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + W * H * 4;
    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = W;
    info.biHeight = -static_cast<LONG>(H); info.biPlanes = 1; info.biBitCount = 32;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    auto srgb = [](float v) -> unsigned char {
        const float s = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
        return static_cast<unsigned char>(std::round(s * 255));
    };
    for (const auto& p : output) {
        const unsigned char rgba[]{srgb(p.b), srgb(p.g), srgb(p.r), 255};
        stream.write(reinterpret_cast<const char*>(rgba), sizeof(rgba));
    }
    Check(stream.good(), "cannot write preview");
}

int main(int argc, char** argv) {
    try {
        Fixture f;
        auto frame = f.Frame();
        const auto hidden = f.Draw(frame);
        Check(hidden[80 * W + 130].r > 0.4f, "hidden contour missing");
        Check(hidden[80 * W + 160].r == f.background[80 * W + 160].r, "silhouette interior filled");
        Check(hidden[80 * W + 129].r == f.background[80 * W + 129].r, "outline escaped actor mask");
        if (argc == 2) WritePreview(hidden, argv[1]);
        auto baselineDepth = f.depth;
        std::fill(f.depth.begin(), f.depth.end(), 0.9f);
        f.Unchanged(f.Draw(frame)); // fully visible
        std::fill(f.depth.begin(), f.depth.end(), 0.7f);
        f.Unchanged(f.Draw(frame)); // equal depth must not self-outline
        f.depth = baselineDepth;
        for (UINT y = 0; y < H; ++y) for (UINT x = 160; x < W; ++x) f.depth[y * W + x] = 0.9f;
        const auto partial = f.Draw(frame);
        Check(partial[80 * W + 130].r > 0.4f, "hidden half missing");
        Check(partial[80 * W + 190].r == f.background[80 * W + 190].r, "visible half outlined");
        Check(partial[80 * W + 159].r == f.background[80 * W + 159].r, "wall intersection outlined");
        f.depth = baselineDepth;
        frame.reversedDepth = true;
        for (auto& d : f.depth) d = 1 - d;
        for (auto& p : f.mask) p.depth = 1 - p.depth;
        const auto reversed = f.Draw(frame);
        Check(std::memcmp(hidden.data(), reversed.data(), hidden.size() * sizeof(Pixel)) == 0,
              "reversed-Z differs");
        frame = f.Frame();
        f.depth = baselineDepth;
        for (auto& p : f.mask) p.depth = 0.7f;
        frame.maskFrame = 6; f.Unchanged(f.Draw(frame, S_FALSE));
        frame = f.Frame(); frame.cameraReady = false; f.Unchanged(f.Draw(frame, S_FALSE));
        frame = f.Frame(); frame.cooperativeSession = false; frame.testMode = false; f.Unchanged(f.Draw(frame, S_FALSE));
        frame = f.Frame(); frame.cooperativeSession = false; frame.testMode = true;
        const auto testModeDrawn = f.Draw(frame);
        Check(std::memcmp(hidden.data(), testModeDrawn.data(), hidden.size() * sizeof(Pixel)) == 0,
              "testMode should allow solo test drawing matching hidden contour");
        frame = f.Frame(); frame.allyMask = nullptr; f.Unchanged(f.Draw(frame, E_INVALIDARG));
        frame = f.Frame(); frame.sceneDepth = frame.allyMask; f.Unchanged(f.Draw(frame, E_INVALIDARG));
        frame = f.Frame();
        for (auto& p : f.mask) p.depth = std::numeric_limits<float>::quiet_NaN();
        f.Unchanged(f.Draw(frame));
        for (auto& p : f.mask) p = {0, 0.7f};
        f.Unchanged(f.Draw(frame)); // no allies, death, disconnect or off-screen
        // A silhouette extending beyond the frame must not draw a screen-edge marker.
        for (UINT y = 40; y < 140; ++y) for (UINT x = 0; x < 40; ++x) f.mask[y * W + x] = {1, 0.7f};
        auto clipped = f.Draw(frame);
        Check(clipped[80 * W].r == f.background[80 * W].r, "screen border marker introduced");
        Check(clipped[80 * W + 39].r > 0.4f, "clipped silhouette edge missing");
        if (f.diagnostics) {
            for (UINT64 i = 0; i < f.diagnostics->GetNumStoredMessages(); ++i) {
                SIZE_T size = 0; HR(f.diagnostics->GetMessage(i, nullptr, &size));
                std::vector<unsigned char> storage(size);
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                HR(f.diagnostics->GetMessage(i, message, &size));
                if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                    std::fprintf(stderr, "%s\n", message->pDescription);
                    throw std::runtime_error("D3D11 debug-layer warning/error");
                }
            }
        }
        std::puts("PASS: WARP outline pixels, occlusion, reversed Z, no wall-intersection edge,");
        std::puts("      stale/session/camera guards, invalid inputs, no screen marker, state restoration.");
        std::puts(f.diagnostics ? "PASS: D3D11 debug layer clean." : "D3D11 debug layer unavailable.");
        std::puts("Synthetic compositor fixture; DS3 native capture is validated separately.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
