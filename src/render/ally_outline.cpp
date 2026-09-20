#include <vector>
#include <new>
#include <cstring>
#include "ally_outline.h"
#include "actor_tracker.h"
#include <cmath>

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ds3sc::render {
namespace {

using Microsoft::WRL::ComPtr;

struct alignas(16) OutlineConstants {
    float dimensions[2];
    float thickness;
    float opacity;
    float outlineColor[3];
    UINT showMaskOnly;
    UINT fillSilhouette;
    float fillOpacity;
    float padding[2];
};
static_assert(sizeof(OutlineConstants) == 48);

struct alignas(16) MarkerConstants {
    float markerPos[8][4];
    float markerColor[4];
    UINT markerCount;
    UINT hasLocalMask;
    float padding[2];
};
static_assert(sizeof(MarkerConstants) == 160);

struct PlayerDiagnosticConstants {
    UINT width;
    UINT height;
    float thickness;
    float outlineOpacity;
    float fillOpacity;
    UINT showFill;
    float alignPad[2];
    float color[3];
    float padding;
};
static_assert(sizeof(PlayerDiagnosticConstants) == 48);

const char kMaskPixelShaderSource[] = R"(
float4 main() : SV_Target {
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
)";

const char kOutlineVertexShaderSource[] = R"(
struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint id : SV_VertexID) {
    VS_OUT o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.uv = uv;
    return o;
}
)";

const char kOutlinePixelShaderSource[] = R"(
Texture2D AllyMask : register(t0);
SamplerState MaskSampler : register(s0);

cbuffer OutlineBuffer : register(b0) {
    float2 Dimensions;
    float  Thickness;
    float  Opacity;
    float3 OutlineColor;
    uint   ShowMaskOnly;
    uint   FillSilhouette;
    float  FillOpacity;
    float2 Padding;
};

struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(VS_OUT pin) : SV_Target {
    float center = AllyMask.SampleLevel(MaskSampler, pin.uv, 0).r;
    if (ShowMaskOnly != 0) {
        return float4(center, center, center, 1.0f);
    }

    float2 texel = 1.0f / Dimensions;
    static const float2 dirs[8] = {
        float2( 1.0f,  0.0f), float2(-1.0f,  0.0f),
        float2( 0.0f,  1.0f), float2( 0.0f, -1.0f),
        float2( 0.7071f,  0.7071f), float2(-0.7071f,  0.7071f),
        float2( 0.7071f, -0.7071f), float2(-0.7071f, -0.7071f)
    };

    float maxNeighbor = 0.0f;
    [unroll]
    for (int i = 0; i < 8; ++i) {
        float s = AllyMask.SampleLevel(MaskSampler, pin.uv + dirs[i] * texel * Thickness, 0).r;
        maxNeighbor = max(maxNeighbor, s);
    }

    float outline = saturate(maxNeighbor - center);
    float alpha = outline * Opacity;

    if (FillSilhouette != 0 && center > 0.5f) {
        alpha = max(alpha, FillOpacity);
    }

    if (alpha <= 0.002f) {
        discard;
    }

    return float4(OutlineColor * alpha, alpha);
}
)";

HRESULT CompileShader(const char* source, const char* target, ID3DBlob** blobOut) noexcept {
    ComPtr<ID3DBlob> errorBlob;
    const HRESULT hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        "main",
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        blobOut,
        &errorBlob
    );
    if (FAILED(hr) && errorBlob) {
        OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
    }
    return hr;
}

} // namespace

AllyOutlineRenderer& AllyOutlineRenderer::Instance() noexcept {
    static AllyOutlineRenderer renderer;
    return renderer;
}

void AllyOutlineRenderer::Reset() noexcept {
    maskTexture_.Reset();
    maskRTV_.Reset();
    maskSRV_.Reset();
    maskPixelShader_.Reset();
    outlineVertexShader_.Reset();
    outlinePixelShader_.Reset();
    outlineConstantBuffer_.Reset();
    maskDepthState_.Reset();
    maskBlendState_.Reset();
    outlineBlendState_.Reset();
    outlineDepthState_.Reset();
    maskSampler_.Reset();
    occludedPixelShader_.Reset();
    occludedWirePixelShader_.Reset();
    occludedDepthStateStd_.Reset();
    occludedDepthStateRev_.Reset();
    occludedBlendState_.Reset();
    occludedRasterizerSolid_.Reset();
    occludedRasterizerWire_.Reset();
    device_.Reset();
    width_ = 0;
    height_ = 0;
}

void AllyOutlineRenderer::ReleaseResolutionResources() noexcept {
    maskTexture_.Reset();
    maskRTV_.Reset();
    maskSRV_.Reset();
    width_ = 0;
    height_ = 0;
}

HRESULT AllyOutlineRenderer::CompileShaders(ID3D11Device* device) noexcept {
    ComPtr<ID3DBlob> maskBlob;
    HRESULT hr = CompileShader(kMaskPixelShaderSource, "ps_4_0", &maskBlob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(maskBlob->GetBufferPointer(), maskBlob->GetBufferSize(), nullptr, &maskPixelShader_);
    if (FAILED(hr)) return hr;

    ComPtr<ID3DBlob> vsBlob;
    hr = CompileShader(kOutlineVertexShaderSource, "vs_4_0", &vsBlob);
    if (FAILED(hr)) return hr;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &outlineVertexShader_);
    if (FAILED(hr)) return hr;

    ComPtr<ID3DBlob> psBlob;
    hr = CompileShader(kOutlinePixelShaderSource, "ps_4_0", &psBlob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &outlinePixelShader_);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(OutlineConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, nullptr, &outlineConstantBuffer_);
    return hr;
}

HRESULT AllyOutlineRenderer::CreateStateObjects(ID3D11Device* device) noexcept {
    // 1. DepthStencil for mask: no depth testing or z-buffer writing
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    HRESULT hr = device->CreateDepthStencilState(&dsd, &maskDepthState_);
    if (FAILED(hr)) return hr;

    // 2. DepthStencil for outline in Present: disabled
    hr = device->CreateDepthStencilState(&dsd, &outlineDepthState_);
    if (FAILED(hr)) return hr;

    // 3. BlendState for mask: overwrites directly
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bd, &maskBlendState_);
    if (FAILED(hr)) return hr;

    // 4. BlendState for outline: pre-multiplied alpha blending
    D3D11_BLEND_DESC obd{};
    obd.RenderTarget[0].BlendEnable = TRUE;
    obd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    obd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    obd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    obd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    obd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    obd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    obd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&obd, &outlineBlendState_);
    if (FAILED(hr)) return hr;

    // 5. RasterizerState
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, &rasterizerState_);
    if (FAILED(hr)) return hr;

    // 6. Sampler
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr = device->CreateSamplerState(&sd, &maskSampler_);
    return hr;
}

void LoadSettingsFromIni(OutlineSettings& settings) noexcept {
    char iniPath[MAX_PATH] = {};
    const HMODULE hMod = GetModuleHandleW(L"ds3sc_companion.dll");
    if (hMod) {
        GetModuleFileNameA(hMod, iniPath, sizeof(iniPath));
        char* lastSlash = std::strrchr(iniPath, '\\');
        if (lastSlash) {
            strcpy_s(lastSlash + 1, sizeof(iniPath) - (lastSlash + 1 - iniPath), "ds3sc_settings.ini");
        }
    }
    if (iniPath[0] == '\0') {
        if (GetFileAttributesA("TheAshenLink\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath, "TheAshenLink\\ds3sc_settings.ini");
        } else if (GetFileAttributesA("SeamplusCoop\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath, "SeamplusCoop\\ds3sc_settings.ini");
        } else {
            strcpy_s(iniPath, "SeamlessCoop\\ds3sc_settings.ini");
        }
    }

    settings.showAllyMask = GetPrivateProfileIntA("OUTLINE", "show_ally_mask", 0, iniPath) != 0;
    settings.showAllyOutline = GetPrivateProfileIntA("OUTLINE", "show_ally_outline", 0, iniPath) != 0;
    settings.fillSilhouette = GetPrivateProfileIntA("OUTLINE", "fill_silhouette", 0, iniPath) != 0;
    settings.wireframeContour = GetPrivateProfileIntA("OUTLINE", "wireframe_contour", 1, iniPath) != 0;
    settings.showFallbackMarkers = GetPrivateProfileIntA("OUTLINE", "show_fallback_markers", 1, iniPath) != 0;

    char buf[64];
    if (GetPrivateProfileStringA("OUTLINE", "thickness", "2.0", buf, sizeof(buf), iniPath) > 0) {
        settings.thickness = static_cast<float>(std::atof(buf));
    }
    if (GetPrivateProfileStringA("OUTLINE", "opacity", "0.75", buf, sizeof(buf), iniPath) > 0) {
        settings.opacity = static_cast<float>(std::atof(buf));
    }
    if (GetPrivateProfileStringA("OUTLINE", "fill_opacity", "0.25", buf, sizeof(buf), iniPath) > 0) {
        settings.fillOpacity = static_cast<float>(std::atof(buf));
    }
    if (GetPrivateProfileStringA("OUTLINE", "color_r", "0.871", buf, sizeof(buf), iniPath) > 0) {
        settings.outlineColor[0] = static_cast<float>(std::atof(buf));
    }
    if (GetPrivateProfileStringA("OUTLINE", "color_g", "0.847", buf, sizeof(buf), iniPath) > 0) {
        settings.outlineColor[1] = static_cast<float>(std::atof(buf));
    }
    if (GetPrivateProfileStringA("OUTLINE", "color_b", "0.791", buf, sizeof(buf), iniPath) > 0) {
        settings.outlineColor[2] = static_cast<float>(std::atof(buf));
    }
}

HRESULT AllyOutlineRenderer::EnsureResources(ID3D11Device* device, UINT width, UINT height) noexcept {
    if (!device || width == 0 || height == 0) return E_INVALIDARG;

    if (device_.Get() != device) {
        Reset();
        device_ = device;
        HRESULT hr = CompileShaders(device);
        if (FAILED(hr)) return hr;
        hr = CreateStateObjects(device);
        if (FAILED(hr)) return hr;
        LoadSettingsFromIni(settings_);
    }

    if (width_ == width && height_ == height && maskRTV_) {
        return S_OK;
    }

    ReleaseResolutionResources();
    width_ = width;
    height_ = height;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &maskTexture_);
    if (FAILED(hr)) return hr;

    hr = device_->CreateRenderTargetView(maskTexture_.Get(), nullptr, &maskRTV_);
    if (FAILED(hr)) return hr;

    hr = device_->CreateShaderResourceView(maskTexture_.Get(), nullptr, &maskSRV_);
    if (FAILED(hr)) return hr;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "[allyMask] Target created: %ux%u (DXGI_FORMAT_R8_UNORM)\n", width, height);
    OutputDebugStringA(buf);

    return S_OK;
}

void AllyOutlineRenderer::ClearMask(ID3D11DeviceContext* ctx) noexcept {
    if (!ctx || !maskRTV_) return;
    const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ctx->ClearRenderTargetView(maskRTV_.Get(), black);
}

HRESULT AllyOutlineRenderer::EnsureOccludedStates(ID3D11Device* device) noexcept {
    if (!device) return E_INVALIDARG;
    if (occludedDepthStateStd_ && occludedPixelShader_ && device_.Get() == device) {
        return S_OK;
    }
    device_ = device;

    // 1. Shaders
    const char kOccludedPS[] = R"(
float4 main() : SV_Target {
    return float4(0.871f, 0.847f, 0.791f, 0.60f);
}
)";
    ComPtr<ID3DBlob> psBlob;
    HRESULT hr = CompileShader(kOccludedPS, "ps_4_0", &psBlob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &occludedPixelShader_);
    if (FAILED(hr)) return hr;

    const char kOccludedWirePS[] = R"(
float4 main() : SV_Target {
    return float4(0.98f, 0.96f, 0.92f, 0.92f);
}
)";
    ComPtr<ID3DBlob> wireBlob;
    hr = CompileShader(kOccludedWirePS, "ps_4_0", &wireBlob);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(wireBlob->GetBufferPointer(), wireBlob->GetBufferSize(), nullptr, &occludedWirePixelShader_);
    if (FAILED(hr)) return hr;

    // 2. Standard DepthStencilState (GREATER): passes ONLY if geometry is behind wall
    D3D11_DEPTH_STENCIL_DESC dsdStd{};
    dsdStd.DepthEnable = TRUE;
    dsdStd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // Does not modify depth buffer
    dsdStd.DepthFunc = D3D11_COMPARISON_GREATER;
    hr = device->CreateDepthStencilState(&dsdStd, &occludedDepthStateStd_);
    if (FAILED(hr)) return hr;

    // 3. Reversed DepthStencilState (LESS): for cameras with reversed z-buffer
    D3D11_DEPTH_STENCIL_DESC dsdRev = dsdStd;
    dsdRev.DepthFunc = D3D11_COMPARISON_LESS;
    hr = device->CreateDepthStencilState(&dsdRev, &occludedDepthStateRev_);
    if (FAILED(hr)) return hr;

    // 4. BlendState: Independent alpha blending (writes exclusively to color RTV0, without touching G-buffer channels)
    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    for (int i = 1; i < 8; ++i) {
        bd.RenderTarget[i].BlendEnable = FALSE;
        bd.RenderTarget[i].RenderTargetWriteMask = 0;
    }
    hr = device->CreateBlendState(&bd, &occludedBlendState_);
    if (FAILED(hr)) return hr;

    // 5. Solid RasterizerState (Cull None)
    D3D11_RASTERIZER_DESC rdSolid{};
    rdSolid.FillMode = D3D11_FILL_SOLID;
    rdSolid.CullMode = D3D11_CULL_NONE;
    rdSolid.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rdSolid, &occludedRasterizerSolid_);
    if (FAILED(hr)) return hr;

    // 6. RasterizerState wireframe (Cull None)
    D3D11_RASTERIZER_DESC rdWire{};
    rdWire.FillMode = D3D11_FILL_WIREFRAME;
    rdWire.CullMode = D3D11_CULL_NONE;
    rdWire.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rdWire, &occludedRasterizerWire_);
    return hr;
}

void AllyOutlineRenderer::DrawOccludedMesh(
    ID3D11DeviceContext* ctx,
    const ReplayDrawParams& params,
    void* originalDrawFn
) noexcept {
    if (!ctx || !originalDrawFn || !settings_.showAllyOutline) {
        if (!settings_.showAllyOutline) {
            OutputDebugStringA("[allyMask][RENDER] DrawOccludedMesh: showAllyOutline is FALSE, skipping\n");
        }
        return;
    }

    // Check if there is an active color render target (ignore shadow-only or pre-z passes)
    ID3D11RenderTargetView* currentRTV = nullptr;
    ID3D11DepthStencilView* currentDSV = nullptr;
    ctx->OMGetRenderTargets(1, &currentRTV, &currentDSV);
    const bool hasRTV = (currentRTV != nullptr);
    if (currentRTV) currentRTV->Release();
    if (currentDSV) currentDSV->Release();
    if (!hasRTV) {
        return;
    }

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return;

    if (!occludedDepthStateStd_ || dev.Get() != device_.Get()) {
        if (FAILED(EnsureOccludedStates(dev.Get()))) return;
    }

    // 1. Query current DepthStencilState to detect z direction
    ID3D11DepthStencilState* savedDSS = nullptr;
    UINT savedStencilRef = 0;
    ctx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);

    bool isReversedDepth = false;
    if (savedDSS) {
        D3D11_DEPTH_STENCIL_DESC desc{};
        savedDSS->GetDesc(&desc);
        if (desc.DepthFunc == D3D11_COMPARISON_GREATER || desc.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL) {
            isReversedDepth = true;
        }
    }

    // 2. Save current states
    ID3D11BlendState* savedBS = nullptr;
    float savedBlendFactor[4]{};
    UINT savedSampleMask = 0;
    ctx->OMGetBlendState(&savedBS, savedBlendFactor, &savedSampleMask);

    ID3D11PixelShader* savedPS = nullptr;
    ctx->PSGetShader(&savedPS, nullptr, nullptr);

    ID3D11RasterizerState* savedRS = nullptr;
    ctx->RSGetState(&savedRS);

    // 3. Set up occluded states
    ctx->OMSetDepthStencilState(isReversedDepth ? occludedDepthStateRev_.Get() : occludedDepthStateStd_.Get(), 0);
    ctx->OMSetBlendState(occludedBlendState_.Get(), nullptr, 0xFFFFFFFF);

    auto callOriginal = [&]() {
        switch (params.type) {
        case DrawCallType::DrawIndexed: {
            using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
            reinterpret_cast<Fn>(originalDrawFn)(ctx, params.indexCount, params.startIndexLocation, params.baseVertexLocation);
            break;
        }
        case DrawCallType::DrawIndexedInstanced: {
            using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
            reinterpret_cast<Fn>(originalDrawFn)(ctx, params.indexCount, params.instanceCount,
                params.startIndexLocation, params.baseVertexLocation, params.startInstanceLocation);
            break;
        }
        case DrawCallType::Draw: {
            using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
            reinterpret_cast<Fn>(originalDrawFn)(ctx, params.vertexCount, params.startVertexLocation);
            break;
        }
        case DrawCallType::DrawInstanced: {
            using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
            reinterpret_cast<Fn>(originalDrawFn)(ctx, params.vertexCount, params.instanceCount,
                params.startVertexLocation, params.startInstanceLocation);
            break;
        }
        }
    };

    // Pass 1: Soft solid silhouette through the wall
    if (settings_.fillSilhouette && occludedPixelShader_ && occludedRasterizerSolid_) {
        ctx->PSSetShader(occludedPixelShader_.Get(), nullptr, 0);
        ctx->RSSetState(occludedRasterizerSolid_.Get());
        callOriginal();
    }

    // Pass 2: Crisp wireframe contour through the wall
    if (settings_.wireframeContour && occludedWirePixelShader_ && occludedRasterizerWire_) {
        ctx->PSSetShader(occludedWirePixelShader_.Get(), nullptr, 0);
        ctx->RSSetState(occludedRasterizerWire_.Get());
        callOriginal();
    }

    allyDrawsThisFrame_++;

    // 4. Restore original states
    ctx->OMSetDepthStencilState(savedDSS, savedStencilRef);
    ctx->OMSetBlendState(savedBS, savedBlendFactor, savedSampleMask);
    ctx->PSSetShader(savedPS, nullptr, 0);
    ctx->RSSetState(savedRS);

    if (savedDSS) savedDSS->Release();
    if (savedBS) savedBS->Release();
    if (savedPS) savedPS->Release();
    if (savedRS) savedRS->Release();
}

void AllyOutlineRenderer::ReplayAllyDraw(
    ID3D11DeviceContext* ctx,
    const ReplayDrawParams& params,
    void* originalDrawFn
) noexcept {
    if (!ctx || !maskRTV_ || !originalDrawFn) return;

    // 1. Save complete D3D11 state that will be temporarily replaced
    D3D11_VIEWPORT savedVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT numSavedVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&numSavedVps, savedVps);

    ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* savedDSV = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);

    ID3D11DepthStencilState* savedDSS = nullptr;
    UINT savedStencilRef = 0;
    ctx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);

    ID3D11BlendState* savedBS = nullptr;
    float savedBlendFactor[4]{};
    UINT savedSampleMask = 0;
    ctx->OMGetBlendState(&savedBS, savedBlendFactor, &savedSampleMask);

    ID3D11PixelShader* savedPS = nullptr;
    ctx->PSGetShader(&savedPS, nullptr, nullptr);

    ID3D11GeometryShader* savedGS = nullptr;
    ctx->GSGetShader(&savedGS, nullptr, nullptr);

    // 2. Configure pipeline to draw white mask without occlusion
    ID3D11RenderTargetView* rtv = maskRTV_.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr); // No DSV
    ctx->OMSetDepthStencilState(maskDepthState_.Get(), 0); // DepthEnable = FALSE
    ctx->OMSetBlendState(maskBlendState_.Get(), nullptr, 0xFFFFFFFF);
    ctx->PSSetShader(maskPixelShader_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);

    // 3. Re-issue draw call with original pose, meshes, and transformations
    switch (params.type) {
    case DrawCallType::DrawIndexed: {
        using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
        reinterpret_cast<Fn>(originalDrawFn)(ctx, params.indexCount, params.startIndexLocation, params.baseVertexLocation);
        break;
    }
    case DrawCallType::DrawIndexedInstanced: {
        using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
        reinterpret_cast<Fn>(originalDrawFn)(ctx, params.indexCount, params.instanceCount,
            params.startIndexLocation, params.baseVertexLocation, params.startInstanceLocation);
        break;
    }
    case DrawCallType::Draw: {
        using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
        reinterpret_cast<Fn>(originalDrawFn)(ctx, params.vertexCount, params.startVertexLocation);
        break;
    }
    case DrawCallType::DrawInstanced: {
        using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
        reinterpret_cast<Fn>(originalDrawFn)(ctx, params.vertexCount, params.instanceCount,
            params.startVertexLocation, params.startInstanceLocation);
        break;
    }
    }

    allyDrawsThisFrame_++;

    // 4. Restore ALL original D3D11 states and release COM references
    ctx->RSSetViewports(numSavedVps, savedVps);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
    ctx->OMSetDepthStencilState(savedDSS, savedStencilRef);
    ctx->OMSetBlendState(savedBS, savedBlendFactor, savedSampleMask);
    ctx->PSSetShader(savedPS, nullptr, 0);
    ctx->GSSetShader(savedGS, nullptr, 0);

    if (savedDSS) savedDSS->Release();
    if (savedBS) savedBS->Release();
    if (savedPS) savedPS->Release();
    if (savedGS) savedGS->Release();
    if (savedDSV) savedDSV->Release();
    for (auto& srtv : savedRTVs) {
        if (srtv) srtv->Release();
    }
}

void AllyOutlineRenderer::ComposeOutline(
    ID3D11DeviceContext* ctx,
    IDXGISwapChain* swapChain
) noexcept {
    frameCount_++;
    if (!ctx || !swapChain || !maskRTV_ || !maskSRV_) return;

    if (!settings_.showAllyOutline && !settings_.showAllyMask) {
        ClearMask(ctx);
        allyDrawsThisFrame_ = 0;
        return;
    }

    // Retrieve backbuffer
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        ClearMask(ctx);
        allyDrawsThisFrame_ = 0;
        return;
    }

    ComPtr<ID3D11RenderTargetView> bbRTV;
    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &bbRTV);
    if (FAILED(hr) || !bbRTV) {
        ClearMask(ctx);
        allyDrawsThisFrame_ = 0;
        return;
    }

    // Update style constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(outlineConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        auto* cb = static_cast<OutlineConstants*>(mapped.pData);
        cb->dimensions[0] = static_cast<float>(width_);
        cb->dimensions[1] = static_cast<float>(height_);
        cb->thickness = std::clamp(settings_.thickness, 0.5f, 10.0f);
        cb->opacity = std::clamp(settings_.opacity, 0.0f, 1.0f);
        cb->outlineColor[0] = settings_.outlineColor[0];
        cb->outlineColor[1] = settings_.outlineColor[1];
        cb->outlineColor[2] = settings_.outlineColor[2];
        cb->showMaskOnly = settings_.showAllyMask ? 1u : 0u;
        cb->fillSilhouette = settings_.fillSilhouette ? 1u : 0u;
        cb->fillOpacity = std::clamp(settings_.fillOpacity, 0.0f, 1.0f);
        cb->padding[0] = 0.0f;
        cb->padding[1] = 0.0f;
        ctx->Unmap(outlineConstantBuffer_.Get(), 0);
    }

    // Save frame state to compose onto backbuffer
    D3D11_VIEWPORT savedVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT numSavedVps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&numSavedVps, savedVps);

    ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* savedDSV = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);

    ID3D11DepthStencilState* savedDSS = nullptr;
    UINT savedStencilRef = 0;
    ctx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);

    ID3D11BlendState* savedBS = nullptr;
    float savedBlendFactor[4]{};
    UINT savedSampleMask = 0;
    ctx->OMGetBlendState(&savedBS, savedBlendFactor, &savedSampleMask);

    ID3D11RasterizerState* savedRS = nullptr;
    ctx->RSGetState(&savedRS);

    ID3D11VertexShader* savedVS = nullptr;
    ctx->VSGetShader(&savedVS, nullptr, nullptr);

    ID3D11PixelShader* savedPS = nullptr;
    ctx->PSGetShader(&savedPS, nullptr, nullptr);

    ID3D11InputLayout* savedIL = nullptr;
    ctx->IAGetInputLayout(&savedIL);

    D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&savedTopology);

    // Bind Backbuffer as destination
    ID3D11RenderTargetView* rtvs[] = { bbRTV.Get() };
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->OMSetBlendState(settings_.showAllyMask ? maskBlendState_.Get() : outlineBlendState_.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(outlineDepthState_.Get(), 0);
    ctx->RSSetState(rasterizerState_.Get());

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(outlineVertexShader_.Get(), nullptr, 0);
    ctx->PSSetShader(outlinePixelShader_.Get(), nullptr, 0);

    ID3D11Buffer* cbs[] = { outlineConstantBuffer_.Get() };
    ctx->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[] = { maskSRV_.Get() };
    ctx->PSSetShaderResources(0, 1, srvs);

    ID3D11SamplerState* samplers[] = { maskSampler_.Get() };
    ctx->PSSetSamplers(0, 1, samplers);

    // Fullscreen triangle
    ctx->Draw(3, 0);

    // Unbind SRV to prevent pipeline hazard conflicts on next frame
    ID3D11ShaderResourceView* nullSRVs[] = { nullptr };
    ctx->PSSetShaderResources(0, 1, nullSRVs);

    // Restore state
    ctx->RSSetViewports(numSavedVps, savedVps);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
    ctx->OMSetDepthStencilState(savedDSS, savedStencilRef);
    ctx->OMSetBlendState(savedBS, savedBlendFactor, savedSampleMask);
    ctx->RSSetState(savedRS);
    ctx->VSSetShader(savedVS, nullptr, 0);
    ctx->PSSetShader(savedPS, nullptr, 0);
    ctx->IASetInputLayout(savedIL);
    ctx->IASetPrimitiveTopology(savedTopology);

    if (savedDSS) savedDSS->Release();
    if (savedBS) savedBS->Release();
    if (savedRS) savedRS->Release();
    if (savedVS) savedVS->Release();
    if (savedPS) savedPS->Release();
    if (savedIL) savedIL->Release();
    if (savedDSV) savedDSV->Release();
    for (auto& srtv : savedRTVs) {
        if (srtv) srtv->Release();
    }

    const auto now = GetTickCount64();
    if (now - lastLogTime_ > 3000) {
        lastLogTime_ = now;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "[allyMask] Outline composed. Ally draws replayed: %u (frame %llu)\n",
            allyDrawsThisFrame_, static_cast<unsigned long long>(frameCount_));
        OutputDebugStringA(buf);
    }

    // Clear mask for next frame
    ClearMask(ctx);
    allyDrawsThisFrame_ = 0;
}

// ---------------------------------------------------------------------------
// AllyOutline (Component for compatibility and render unit tests)
// ---------------------------------------------------------------------------

namespace {
struct alignas(16) WarpConstants {
    UINT width, height;
    float thickness, depthBias;
    float white[3];
    float opacity;
    UINT reversedDepth;
    UINT visibleOutline, showMask;
    UINT fillSilhouette;
    float fillOpacity;
    float padding[3];
};
static_assert(sizeof(WarpConstants) == 64);

const char kAllyOutlineHLSL[] = R"(
Texture2D<float2> AllyMask : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float2> LocalMask : register(t2);

cbuffer OutlineStyle : register(b0)
{
    uint2 Dimensions;
    float Thickness;
    float DepthBias;
    float3 AshWhite;
    float Opacity;
    uint ReversedDepth;
    uint VisibleOutline;
    uint ShowMask;
    uint FillSilhouette;
    float FillOpacity;
    float3 Padding;
};

float4 FullscreenVS(uint vertex : SV_VertexID) : SV_Position
{
    float2 p = float2((vertex << 1) & 2, vertex & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

bool OnScreen(int2 p)
{
    return all(p >= 0) && all(p < int2(Dimensions));
}

float Coverage(int2 p)
{
    if (!OnScreen(p)) return 1;
    return saturate(AllyMask.Load(int3(p, 0)).x);
}

float4 OutlinePS(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float2 ally = AllyMask.Load(int3(p, 0));
    float scene = SceneDepth.Load(int3(p, 0));
    if (!all(isfinite(ally)) || !isfinite(scene) || ally.x <= 0 ||
        ally.y < 0 || ally.y > 1 || scene < 0 || scene > 1) discard;

    if ((ReversedDepth & 2) != 0 && (ReversedDepth & 1) == 0) ally.y = 1 - ally.y;
    // Only the visible local player's surface blocks the ally overlay. A wall
    // in front of both actors must still reveal the ally through that wall.
    if ((ReversedDepth & 4) != 0)
    {
        float2 local = LocalMask.Load(int3(p, 0));
        if (local.x > 0 && all(isfinite(local)) && local.y >= 0 && local.y <= 1)
        {
            if ((ReversedDepth & 2) != 0 && (ReversedDepth & 1) == 0) local.y = 1 - local.y;
            float localBehindScene = (ReversedDepth & 1) != 0 ? scene - local.y : local.y - scene;
            float allyBehindLocal = (ReversedDepth & 1) != 0 ? local.y - ally.y : ally.y - local.y;
            if (localBehindScene <= DepthBias && allyBehindLocal > DepthBias) discard;
        }
    }
    float behind = (ReversedDepth & 1) != 0 ? scene - ally.y : ally.y - scene;
    if (ShowMask != 0) return float4(AshWhite * Opacity, Opacity);
    if (VisibleOutline == 0 && behind <= DepthBias) discard;

    const int2 directions[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };
    float edge = 0;
    [loop] for (int radius = 1; radius <= 6; ++radius)
    {
        [unroll] for (int i = 0; i < 8; ++i)
        {
            float distance = radius * (i < 4 ? 1.0 : 1.41421356);
            float falloff = 1 - smoothstep(Thickness, Thickness + 1.25, distance);
            if (falloff > 0)
                edge = max(edge, (1 - Coverage(p + radius * directions[i])) * falloff);
        }
    }
    float alpha = saturate(ally.x) * edge * Opacity;
    if (FillSilhouette != 0 && behind > DepthBias)
    {
        alpha = max(alpha, saturate(ally.x) * FillOpacity);
    }
    if (alpha <= 0) discard;
    return float4(AshWhite * alpha, alpha);
}
)";

bool TextureView(ID3D11View* view, ID3D11Device* device,
                 ComPtr<ID3D11Texture2D>& texture, D3D11_TEXTURE2D_DESC& desc) {
    if (!view) return false;
    ComPtr<ID3D11Device> owner;
    view->GetDevice(&owner);
    if (owner.Get() != device) return false;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (FAILED(resource.As(&texture))) return false;
    texture->GetDesc(&desc);
    return desc.SampleDesc.Count == 1 && desc.ArraySize == 1 && desc.MipLevels == 1;
}
}

void AllyOutline::Reset() noexcept {
    recording_.Reset(); vertex_.Reset(); pixel_.Reset(); constants_.Reset();
    blend_.Reset(); depth_.Reset(); raster_.Reset(); device_.Reset();
}

HRESULT AllyOutline::Initialize(ID3D11Device* device) noexcept {
    Reset();
    if (!device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return E_INVALIDARG;
    device_ = device;
    auto initialize = [&]() -> HRESULT {
        HRESULT hr = device->CreateDeferredContext(0, &recording_);
        if (FAILED(hr)) return hr;

        ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
        hr = D3DCompile(kAllyOutlineHLSL, sizeof(kAllyOutlineHLSL), nullptr, nullptr, nullptr, "FullscreenVS", "vs_5_0", 0, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) return hr;
        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertex_);
        if (FAILED(hr)) return hr;

        hr = D3DCompile(kAllyOutlineHLSL, sizeof(kAllyOutlineHLSL), nullptr, nullptr, nullptr, "OutlinePS", "ps_5_0", 0, 0, &psBlob, &errBlob);
        if (FAILED(hr)) return hr;
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixel_);
        if (FAILED(hr)) return hr;

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(WarpConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&cb, nullptr, &constants_);
        if (FAILED(hr)) return hr;

        D3D11_BLEND_DESC blend{};
        auto& rt = blend.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
            D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        hr = device->CreateBlendState(&blend, &blend_);
        if (FAILED(hr)) return hr;

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = device->CreateDepthStencilState(&depth, &depth_);
        if (FAILED(hr)) return hr;

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        return device->CreateRasterizerState(&raster, &raster_);
    };
    const HRESULT hr = initialize();
    if (FAILED(hr)) Reset();
    return hr;
}

HRESULT AllyOutline::Draw(ID3D11DeviceContext* immediate, const OutlineFrame& frame) noexcept {
    if (!device_ || !immediate || immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return E_INVALIDARG;
    ComPtr<ID3D11Device> owner;
    immediate->GetDevice(&owner);
    if (owner.Get() != device_.Get()) return E_INVALIDARG;
    if ((!frame.cooperativeSession && !frame.testMode) || !frame.cameraReady || frame.frame == 0 ||
        frame.maskFrame != frame.frame || frame.depthFrame != frame.frame) return S_FALSE;
    ComPtr<ID3D11Texture2D> mask, scene, target;
    D3D11_TEXTURE2D_DESC m{}, s{}, t{};
    if (!TextureView(frame.allyMask, device_.Get(), mask, m) ||
        !TextureView(frame.sceneDepth, device_.Get(), scene, s) ||
        !TextureView(frame.target, device_.Get(), target, t)) return E_INVALIDARG;
    if (mask == scene || mask == target || scene == target ||
        m.Width != s.Width || m.Height != s.Height ||
        m.Width != t.Width || m.Height != t.Height) return E_INVALIDARG;
    D3D11_SHADER_RESOURCE_VIEW_DESC mv{}, sv{};
    frame.allyMask->GetDesc(&mv); frame.sceneDepth->GetDesc(&sv);
    D3D11_RENDER_TARGET_VIEW_DESC tv{};
    frame.target->GetDesc(&tv);
    if (mv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
        tv.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
        mv.Format != DXGI_FORMAT_R32G32_FLOAT ||
        (sv.Format != DXGI_FORMAT_R32_FLOAT && sv.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
         sv.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS))
        return E_INVALIDARG;
    if (tv.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        tv.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        tv.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        tv.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        tv.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
        tv.Format != DXGI_FORMAT_R32G32B32A32_FLOAT) return E_INVALIDARG;

    if (frame.localMask) {
        ComPtr<ID3D11Texture2D> local; D3D11_TEXTURE2D_DESC l{};
        D3D11_SHADER_RESOURCE_VIEW_DESC lv{}; frame.localMask->GetDesc(&lv);
        if (!TextureView(frame.localMask, device_.Get(), local, l) ||
            l.Width != m.Width || l.Height != m.Height || local == target || local == scene || local == mask ||
            lv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || lv.Format != DXGI_FORMAT_R32G32_FLOAT)
            return E_INVALIDARG;
    }
    WarpConstants style{m.Width, m.Height, std::clamp(m.Height / 540.0f, 1.0f, 4.0f),
        0.00002f, {frame.fillSilhouette ? 0.95f : 0.871f, frame.fillSilhouette ? 0.95f : 0.847f, frame.fillSilhouette ? 1.0f : 0.791f},
        0.85f, (frame.reversedDepth ? 1u : 0u) | (frame.encodedNearDepth ? 2u : 0u) | (frame.localMask ? 4u : 0u),
        frame.visibleOutline ? 1u : 0u, frame.showMask ? 1u : 0u,
        frame.fillSilhouette ? 1u : 0u,
        frame.fillOpacity > 0.0f ? frame.fillOpacity : 0.30f,
        {0.0f, 0.0f, 0.0f}};
    if (std::isfinite(frame.thickness) && frame.thickness > 0)
        style.thickness = std::clamp(frame.thickness, 1.0f, 4.0f);
    recording_->ClearState();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = recording_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    *static_cast<WarpConstants*>(mapped.pData) = style;
    recording_->Unmap(constants_.Get(), 0);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(m.Width), static_cast<float>(m.Height), 0, 1};
    recording_->RSSetViewports(1, &viewport);
    recording_->RSSetState(raster_.Get());
    recording_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    recording_->VSSetShader(vertex_.Get(), nullptr, 0);
    recording_->PSSetShader(pixel_.Get(), nullptr, 0);
    ID3D11Buffer* cb = constants_.Get();
    recording_->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* inputs[]{frame.allyMask, frame.sceneDepth,
        frame.localMask ? frame.localMask : frame.allyMask};
    recording_->PSSetShaderResources(0, 3, inputs);
    recording_->OMSetRenderTargets(1, &frame.target, nullptr);
    recording_->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    recording_->OMSetDepthStencilState(depth_.Get(), 0);
    recording_->Draw(3, 0);
    ComPtr<ID3D11CommandList> commands;
    hr = recording_->FinishCommandList(FALSE, &commands);
    if (FAILED(hr)) { recording_->ClearState(); return hr; }
    immediate->ExecuteCommandList(commands.Get(), TRUE);
    return device_->GetDeviceRemovedReason();
}

extern "C" {
__declspec(dllexport) volatile LONG ds3scOutlineCaptureAttempts = 0;
// 0=accepted, 1=context, 2=viewport, 3=geometry, 4=depth, 5=scene/copy.
__declspec(dllexport) volatile LONG ds3scOutlineCaptureRejectReason = 0;
__declspec(dllexport) volatile LONG ds3scOutlineComposeResult = S_FALSE;
}

namespace {
// Predication affects copies and clears as well as draws. Preserve it around
// every private GPU operation, so occlusion cannot suppress depth snapshots or
// leave last frame's mask behind.
struct Unpredicated {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11Predicate> predicate;
    BOOL value = FALSE;
    explicit Unpredicated(ID3D11DeviceContext* ctx) : context(ctx) {
        context->GetPredication(&predicate, &value); context->SetPredication(nullptr, FALSE);
    }
    ~Unpredicated() { context->SetPredication(predicate.Get(), value); }
};
const GUID kOutlineCommands{0x248677b2,0xc15a,0x45eb,{0xb6,0x69,0x0e,0x6a,0xfd,0x48,0xd9,0x29}};
struct CommandEvent {
    UINT count = 0;
    bool reverse = false, localPlayer = false;
    ComPtr<ID3D11Texture2D> source, depth;
    ComPtr<ID3D11ShaderResourceView> view;
};
struct OutlineCommands final : IUnknown {
    std::atomic<ULONG> refs{1};
    std::uint64_t generation = 0;
    ComPtr<ID3D11Texture2D> captureSource;
    std::vector<CommandEvent> events;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto value = --refs; if (!value) delete this; return value; }
};
ComPtr<OutlineCommands> GetCommands(ID3D11DeviceChild* object) {
    ComPtr<OutlineCommands> result;
    if (object) { UINT bytes = sizeof(void*); object->GetPrivateData(kOutlineCommands, &bytes, result.GetAddressOf()); }
    return result;
}
ComPtr<OutlineCommands> Pending(ID3D11DeviceContext* ctx, std::uint64_t generation) {
    auto result = GetCommands(ctx);
    if (!result || result->generation != generation) {
        result.Attach(new(std::nothrow) OutlineCommands);
        if (!result) return {};
        result->generation = generation;
        if (FAILED(ctx->SetPrivateDataInterface(kOutlineCommands, result.Get()))) return {};
    }
    return result;
}
}

LiveAllyOutline& LiveAllyOutline::Instance() noexcept {
    static LiveAllyOutline instance;
    return instance;
}

void LiveAllyOutline::Reset() noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    outline_.Reset();
    mask_.Reset(); scene_.Reset(); sceneCopy_.Reset();
    localMask_.Reset(); localTarget_.Reset(); localView_.Reset(); localCaptures_ = lastLocalCaptures_ = 0;
    persistentSceneCopy_.Reset(); persistentSceneView_.Reset();
    ++generation_;
    maskTarget_.Reset(); maskView_.Reset(); sceneView_.Reset();
    captureShader_.Reset(); captureReversed_.Reset(); captureDepth_.Reset(); overwrite_.Reset();
    markerVertexShader_.Reset(); markerPixelShader_.Reset(); markerConstantBuffer_.Reset();
    markerBlendState_.Reset(); markerDepthState_.Reset(); markerRaster_.Reset();
    playerVertexShader_.Reset(); playerPixelShader_.Reset(); playerConstantBuffer_.Reset();
    playerBlendState_.Reset(); playerDepthState_.Reset(); playerRaster_.Reset();
    device_.Reset(); width_ = height_ = captures_ = 0;
    sceneFrozen_ = mixedScene_ = false;
    for (auto& clear : depthClears_) clear.texture.Reset();
    nextClear_ = 0;
}

HRESULT LiveAllyOutline::EnsureInitialized(ID3D11Device* device, UINT width, UINT height) noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    if (!device || width == 0 || height == 0) return E_INVALIDARG;
    if (device_.Get() == device && width_ == width && height_ == height && mask_) return S_OK;
    return Initialize(device, width, height);
}

HRESULT LiveAllyOutline::RenderPlayerDiagnostic(ID3D11DeviceContext* context,
                                                ID3D11RenderTargetView* targetRTV,
                                                bool fillSilhouette,
                                                float thickness) noexcept {
    if (!context || !targetRTV || !localView_ || localCaptures_ == 0 ||
        !playerVertexShader_ || !playerPixelShader_ || !playerConstantBuffer_) {
        return S_FALSE;
    }

    PlayerDiagnosticConstants cb{};
    cb.width = width_;
    cb.height = height_;
    cb.thickness = std::clamp(std::isfinite(thickness) ? thickness : 2.0f, 1.0f, 4.0f);
    cb.outlineOpacity = 0.95f;
    cb.fillOpacity = 0.35f;
    (void)fillSilhouette;
    cb.showFill = 1u;
    // Magenta is deliberately distinct from the ash-white ally marker.
    cb.color[0] = 1.0f;
    cb.color[1] = 0.08f;
    cb.color[2] = 0.90f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(playerConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return E_FAIL;
    }
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    context->Unmap(playerConstantBuffer_.Get(), 0);

    ComPtr<ID3D11RenderTargetView> prevRtv;
    ComPtr<ID3D11DepthStencilView> prevDsv;
    context->OMGetRenderTargets(1, &prevRtv, &prevDsv);

    ComPtr<ID3D11BlendState> prevBlend;
    FLOAT prevBlendFactor[4]{};
    UINT prevSampleMask = 0;
    context->OMGetBlendState(&prevBlend, prevBlendFactor, &prevSampleMask);

    ComPtr<ID3D11DepthStencilState> prevDepth;
    UINT prevStencilRef = 0;
    context->OMGetDepthStencilState(&prevDepth, &prevStencilRef);

    ComPtr<ID3D11RasterizerState> prevRaster;
    context->RSGetState(&prevRaster);
    UINT viewportCount = 1;
    D3D11_VIEWPORT prevViewport{};
    context->RSGetViewports(&viewportCount, &prevViewport);

    ComPtr<ID3D11VertexShader> prevVS;
    ComPtr<ID3D11PixelShader> prevPS;
    context->VSGetShader(&prevVS, nullptr, nullptr);
    context->PSGetShader(&prevPS, nullptr, nullptr);
    ComPtr<ID3D11Buffer> prevPsCb;
    context->PSGetConstantBuffers(0, 1, &prevPsCb);
    ComPtr<ID3D11ShaderResourceView> prevPsSrv;
    context->PSGetShaderResources(0, 1, &prevPsSrv);
    D3D11_PRIMITIVE_TOPOLOGY prevTopology{};
    context->IAGetPrimitiveTopology(&prevTopology);

    D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(playerRaster_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(playerVertexShader_.Get(), nullptr, 0);
    context->PSSetShader(playerPixelShader_.Get(), nullptr, 0);
    ID3D11Buffer* cbBuffer = playerConstantBuffer_.Get();
    context->PSSetConstantBuffers(0, 1, &cbBuffer);
    ID3D11ShaderResourceView* localSrv = localView_.Get();
    context->PSSetShaderResources(0, 1, &localSrv);
    context->OMSetRenderTargets(1, &targetRTV, nullptr);
    context->OMSetBlendState(playerBlendState_.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(playerDepthState_.Get(), 0);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(0, 1, &nullSrv);
    context->RSSetViewports(viewportCount, &prevViewport);
    context->RSSetState(prevRaster.Get());
    context->IASetPrimitiveTopology(prevTopology);
    context->VSSetShader(prevVS.Get(), nullptr, 0);
    context->PSSetShader(prevPS.Get(), nullptr, 0);
    ID3D11Buffer* prevCb = prevPsCb.Get();
    context->PSSetConstantBuffers(0, 1, &prevCb);
    ID3D11ShaderResourceView* prevSrv = prevPsSrv.Get();
    context->PSSetShaderResources(0, 1, &prevSrv);
    context->OMSetRenderTargets(1, &prevRtv, prevDsv.Get());
    context->OMSetBlendState(prevBlend.Get(), prevBlendFactor, prevSampleMask);
    context->OMSetDepthStencilState(prevDepth.Get(), prevStencilRef);
    return S_OK;
}

HRESULT LiveAllyOutline::Initialize(ID3D11Device* device, UINT width, UINT height) noexcept {
    Reset();
    device_ = device; width_ = width; height_ = height;
    auto initialize = [&]() -> HRESULT {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &mask_);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = device->CreateRenderTargetView(mask_.Get(), nullptr, &maskTarget_))) return hr;
        if (FAILED(hr = device->CreateShaderResourceView(mask_.Get(), nullptr, &maskView_))) return hr;
        if (FAILED(hr = device->CreateTexture2D(&desc, nullptr, &localMask_))) return hr;
        if (FAILED(hr = device->CreateRenderTargetView(localMask_.Get(), nullptr, &localTarget_))) return hr;
        if (FAILED(hr = device->CreateShaderResourceView(localMask_.Get(), nullptr, &localView_))) return hr;
        // Commutative accumulation: near depth is MAX(z) for reverse-Z and
        // MAX(1-z) for conventional Z. No per-command-list clear or shared DSV.
        const char* shaders[]{
            "float2 main(float4 p:SV_Position):SV_Target {return float2(1,1-p.z);}",
            "float2 main(float4 p:SV_Position):SV_Target {return float2(1,p.z);}"
        };
        ID3D11PixelShader** outputs[]{captureShader_.GetAddressOf(), captureReversed_.GetAddressOf()};
        for (int i=0;i<2;++i) {
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(hr = D3DCompile(shaders[i], strlen(shaders[i]), nullptr, nullptr, nullptr,
                "main", "ps_5_0", 0, 0, &code, &errors))) return hr;
            if (FAILED(hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                nullptr, outputs[i]))) return hr;
        }
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(hr = device->CreateDepthStencilState(&depth, &captureDepth_))) return hr;
        D3D11_BLEND_DESC blend{};
        auto& rt = blend.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.DestBlend = rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = device->CreateBlendState(&blend, &overwrite_))) return hr;

        static const char kMarkerVS[] =
            "float4 main(uint id : SV_VertexID) : SV_Position {\n"
            "    float2 uv = float2((id << 1) & 2, id & 2);\n"
            "    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
            "}\n";

        static const char kMarkerPS[] =
            "Texture2D<float2> LocalMask : register(t0);\n"
            "cbuffer MarkerConstants : register(b0) {\n"
            "    float4 MarkerPos[8];\n"
            "    float4 MarkerColor;\n"
            "    uint   MarkerCount;\n"
            "    uint   HasLocalMask;\n"
            "    float2 MarkerPadding;\n"
            "};\n"
            "float4 main(float4 pos : SV_Position) : SV_Target {\n"
            "    if (HasLocalMask != 0) {\n"
            "        float2 local = LocalMask.Load(int3(pos.xy, 0));\n"
            "        if (local.x > 0.0f) discard;\n"
            "    }\n"
            "    float2 p = pos.xy;\n"
            "    float maxAlpha = 0.0f;\n"
            "    [loop] for (uint i = 0; i < MarkerCount; ++i) {\n"
            "        float2 diff = p - MarkerPos[i].xy;\n"
            "        float diamond = abs(diff.x) + abs(diff.y);\n"
            "        float border = 1.0f - saturate(abs(diamond - 9.0f) / 1.5f);\n"
            "        float center = 1.0f - saturate(length(diff) / 2.5f);\n"
            "        float glow = saturate(1.0f - diamond / 16.0f) * 0.25f;\n"
            "        float marker = max(max(border, center), glow);\n"
            "        maxAlpha = max(maxAlpha, marker * MarkerColor.a);\n"
            "    }\n"
            "    if (maxAlpha <= 0.005f) discard;\n"
            "    return float4(MarkerColor.rgb * maxAlpha, maxAlpha);\n"
            "}\n";

        ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
        hr = D3DCompile(kMarkerVS, strlen(kMarkerVS), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) return hr;
        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &markerVertexShader_);
        if (FAILED(hr)) return hr;

        hr = D3DCompile(kMarkerPS, strlen(kMarkerPS), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
        if (FAILED(hr)) return hr;
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &markerPixelShader_);
        if (FAILED(hr)) return hr;

        D3D11_BUFFER_DESC markerCB{};
        markerCB.ByteWidth = sizeof(MarkerConstants);
        markerCB.Usage = D3D11_USAGE_DYNAMIC;
        markerCB.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        markerCB.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&markerCB, nullptr, &markerConstantBuffer_);
        if (FAILED(hr)) return hr;

        D3D11_BLEND_DESC markerBlend{};
        auto& mb = markerBlend.RenderTarget[0];
        mb.BlendEnable = TRUE;
        mb.SrcBlend = D3D11_BLEND_ONE;
        mb.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        mb.BlendOp = D3D11_BLEND_OP_ADD;
        mb.SrcBlendAlpha = D3D11_BLEND_ZERO;
        mb.DestBlendAlpha = D3D11_BLEND_ONE;
        mb.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        mb.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&markerBlend, &markerBlendState_);
        if (FAILED(hr)) return hr;

        D3D11_DEPTH_STENCIL_DESC markerDepth{};
        markerDepth.DepthEnable = FALSE;
        markerDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        markerDepth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = device->CreateDepthStencilState(&markerDepth, &markerDepthState_);
        if (FAILED(hr)) return hr;

        D3D11_RASTERIZER_DESC markerRast{};
        markerRast.FillMode = D3D11_FILL_SOLID;
        markerRast.CullMode = D3D11_CULL_NONE;
        markerRast.DepthClipEnable = TRUE;
        hr = device->CreateRasterizerState(&markerRast, &markerRaster_);
        if (FAILED(hr)) return hr;

        // Diagnostic shader: display the local player's actual captured
        // render silhouette, with a visible edge so marker overlap can be
        // inspected without relying on the collision capsule.
        static const char kPlayerDebugPS[] = R"(
Texture2D<float2> LocalMask : register(t0);
cbuffer PlayerDiagnostic : register(b0) {
    uint2 Dimensions;
    float Thickness;
    float OutlineOpacity;
    float FillOpacity;
    uint ShowFill;
    float2 AlignPad;
    float3 DebugColor;
    float Padding;
};

bool OnScreen(int2 p) { return all(p >= 0) && all(p < int2(Dimensions)); }
float Coverage(int2 p) {
    if (!OnScreen(p)) return 0.0f;
    return saturate(LocalMask.Load(int3(p, 0)).x);
}

float4 main(float4 position : SV_Position) : SV_Target {
    int2 p = int2(position.xy);
    float localCov = Coverage(p);

    const int2 directions[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };
    float edge = 0.0f;
    [loop] for (int radius = 1; radius <= 6; ++radius) {
        [unroll] for (int i = 0; i < 8; ++i) {
            float distance = radius * (i < 4 ? 1.0f : 1.41421356f);
            float falloff = 1.0f - smoothstep(Thickness, Thickness + 1.25f, distance);
            if (falloff > 0.0f) {
                float neighborCov = Coverage(p + radius * directions[i]);
                edge = max(edge, abs(localCov - neighborCov) * falloff);
            }
        }
    }

    float alpha = edge * OutlineOpacity;
    if (ShowFill != 0 && localCov > 0.0f) {
        alpha = max(alpha, FillOpacity);
    }
    if (alpha <= 0.005f) discard;
    return float4(DebugColor * alpha, alpha);
}
)";

        ComPtr<ID3DBlob> playerPsBlob, playerErrBlob;
        hr = D3DCompile(kMarkerVS, strlen(kMarkerVS), nullptr, nullptr, nullptr,
                        "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) return hr;
        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                        nullptr, &playerVertexShader_);
        if (FAILED(hr)) return hr;
        hr = D3DCompile(kPlayerDebugPS, strlen(kPlayerDebugPS), nullptr, nullptr, nullptr,
                        "main", "ps_5_0", 0, 0, &playerPsBlob, &playerErrBlob);
        if (FAILED(hr)) return hr;
        hr = device->CreatePixelShader(playerPsBlob->GetBufferPointer(), playerPsBlob->GetBufferSize(),
                                       nullptr, &playerPixelShader_);
        if (FAILED(hr)) return hr;

        D3D11_BUFFER_DESC playerCb{};
        playerCb.ByteWidth = sizeof(PlayerDiagnosticConstants);
        playerCb.Usage = D3D11_USAGE_DYNAMIC;
        playerCb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        playerCb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&playerCb, nullptr, &playerConstantBuffer_);
        if (FAILED(hr)) return hr;

        D3D11_BLEND_DESC playerBlend{};
        auto& pb = playerBlend.RenderTarget[0];
        pb.BlendEnable = TRUE;
        pb.SrcBlend = D3D11_BLEND_ONE;
        pb.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        pb.BlendOp = D3D11_BLEND_OP_ADD;
        pb.SrcBlendAlpha = D3D11_BLEND_ZERO;
        pb.DestBlendAlpha = D3D11_BLEND_ONE;
        pb.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        pb.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&playerBlend, &playerBlendState_);
        if (FAILED(hr)) return hr;

        D3D11_DEPTH_STENCIL_DESC playerDepth{};
        playerDepth.DepthEnable = FALSE;
        playerDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        playerDepth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = device->CreateDepthStencilState(&playerDepth, &playerDepthState_);
        if (FAILED(hr)) return hr;

        hr = device->CreateRasterizerState(&markerRast, &playerRaster_);
        if (FAILED(hr)) return hr;

        ComPtr<ID3D11DeviceContext> immediate; device->GetImmediateContext(&immediate);
        Unpredicated unpredicated(immediate.Get());
        const float clear[4]{}; immediate->ClearRenderTargetView(maskTarget_.Get(), clear);
        immediate->ClearRenderTargetView(localTarget_.Get(), clear);
        return outline_.Initialize(device);
    };
    const auto hr = initialize();
    if (FAILED(hr)) Reset();
    return hr;
}

HRESULT LiveAllyOutline::PrepareSceneCopy(ID3D11Texture2D* source) noexcept {
    D3D11_TEXTURE2D_DESC desc{}, previous{};
    source->GetDesc(&desc);
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
        desc.Format = DXGI_FORMAT_R32_TYPELESS; format = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS; format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
        desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS; format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    default: return E_INVALIDARG;
    }
    if (persistentSceneCopy_ && persistentSceneView_) {
        persistentSceneCopy_->GetDesc(&previous);
        if (previous.Format == desc.Format && previous.Width == desc.Width &&
            previous.Height == desc.Height) {
            sceneCopy_ = persistentSceneCopy_;
            sceneView_ = persistentSceneView_;
            return S_OK;
        }
    }
    persistentSceneCopy_.Reset(); persistentSceneView_.Reset();
    sceneCopy_.Reset(); sceneView_.Reset();
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &persistentSceneCopy_);
    if (FAILED(hr)) return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(persistentSceneCopy_.Get(), &view, &persistentSceneView_);
    if (FAILED(hr)) { persistentSceneCopy_.Reset(); return hr; }
    sceneCopy_ = persistentSceneCopy_;
    sceneView_ = persistentSceneView_;
    return hr;
}

bool LiveAllyOutline::Capture(ID3D11DeviceContext* ctx, const ReplayDrawParams& params,
                              void* original, bool localPlayer) noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    InterlockedIncrement(&ds3scOutlineCaptureAttempts);
    ds3scOutlineCaptureRejectReason = 1;
    // Resources and camera dimensions belong to the presented swapchain. A
    // shadow/reflection viewport must never resize or replace the main mask.
    if (!ctx || !original || !device_ ||
        (ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE && sceneFrozen_)) return false;
    ComPtr<ID3D11Device> owner; ctx->GetDevice(&owner);
    if (owner != device_) return false;
    ds3scOutlineCaptureRejectReason = 2;
    D3D11_VIEWPORT viewport{}; UINT viewportCount = 1;
    ctx->RSGetViewports(&viewportCount, &viewport);
    if (viewportCount < 1 || viewport.Width < 64.0f || viewport.Height < 64.0f) return false;
    if (viewportCount != 1 ||
        std::abs(viewport.Width - static_cast<float>(width_)) > 32.0f ||
        std::abs(viewport.Height - static_cast<float>(height_)) > 32.0f ||
        viewport.TopLeftX != 0 || viewport.TopLeftY != 0 ||
        viewport.MinDepth != 0 || viewport.MaxDepth != 1) return false;
    ds3scOutlineCaptureRejectReason = 3;
    // A packet identifies one actor. A mixed engine instance batch cannot be
    // attributed wholesale to it without a verified per-instance owner table.
    if ((params.type == DrawCallType::DrawInstanced || params.type == DrawCallType::DrawIndexedInstanced) &&
        params.instanceCount != 1) return false;
    D3D11_PRIMITIVE_TOPOLOGY topology{}; ctx->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) return false;
    ds3scOutlineCaptureRejectReason = 4;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11RenderTargetView> colorTarget;
    ctx->OMGetRenderTargets(1, &colorTarget, &depthView);
    if (!colorTarget) return false; // Depth-only shadow passes are not the scene.
    // Replaying a geometry shader with stream output would mutate game buffers.
    ID3D11Buffer* streamOutput[D3D11_SO_BUFFER_SLOT_COUNT]{};
    ctx->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, streamOutput);
    bool hasStreamOutput = false;
    for (auto* buffer : streamOutput) if (buffer) { hasStreamOutput = true; buffer->Release(); }
    if (hasStreamOutput) return false;
    ComPtr<ID3D11Texture2D> depthTexture; D3D11_TEXTURE2D_DESC desc{};
    if (!TextureView(depthView.Get(), device_.Get(), depthTexture, desc) ||
        desc.Width != width_ || desc.Height != height_) return false;
    ComPtr<ID3D11DepthStencilState> depthState; UINT reference = 0;
    ctx->OMGetDepthStencilState(&depthState, &reference);
    D3D11_DEPTH_STENCIL_DESC state{};
    state.DepthEnable = TRUE; state.DepthFunc = D3D11_COMPARISON_LESS;
    if (depthState) depthState->GetDesc(&state);
    if (!state.DepthEnable || state.DepthFunc == D3D11_COMPARISON_ALWAYS ||
        state.DepthFunc == D3D11_COMPARISON_NEVER || state.DepthFunc == D3D11_COMPARISON_NOT_EQUAL) return false;
    bool reverse = state.DepthFunc == D3D11_COMPARISON_GREATER || state.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
    if (state.DepthFunc == D3D11_COMPARISON_EQUAL) {
        bool known = false;
        for (const auto& clear : depthClears_) if (clear.texture == depthTexture) {
            reverse = clear.depth == 0.0f;
            known = clear.depth == 0.0f || clear.depth == 1.0f;
            break;
        }
        if (!known) return false; // EQUAL alone does not identify a depth convention.
    }
    ds3scOutlineCaptureRejectReason = 5;
    auto pending = ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? Pending(ctx, generation_) : nullptr;
    if (ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED && !pending) return false;
    if (pending) {
        if (pending->captureSource && pending->captureSource != depthTexture) return false;
        try {
            if (!pending->events.empty() && pending->events.back().count && pending->events.back().reverse == reverse &&
                pending->events.back().localPlayer == localPlayer)
                ++pending->events.back().count;
            else {
                CommandEvent event; event.count = 1; event.reverse = reverse; event.source = depthTexture; event.localPlayer = localPlayer;
                pending->events.push_back(std::move(event));
            }
            pending->captureSource = depthTexture;
        } catch (...) { return false; }
    } else {
        if (scene_ && scene_ != depthTexture) return false;
        if (FAILED(PrepareSceneCopy(depthTexture.Get()))) return false;
        scene_ = depthTexture;
    }
    ComPtr<ID3D11Predicate> predicate; BOOL predicateValue = FALSE;
    ctx->GetPredication(&predicate, &predicateValue);
    ctx->SetPredication(nullptr, FALSE);
    // Restore every state changed by replay, including all MRTs and class instances.
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, nullptr);
    ComPtr<ID3D11BlendState> blend; float factor[4]{}; UINT sampleMask = 0;
    ctx->OMGetBlendState(&blend, factor, &sampleMask);
    ComPtr<ID3D11PixelShader> pixel;
    ID3D11ClassInstance* instances[256]{}; UINT instanceCount = 256;
    ctx->PSGetShader(&pixel, instances, &instanceCount);
    auto* mask = localPlayer ? localTarget_.Get() : maskTarget_.Get();
    ctx->OMSetRenderTargets(1, &mask, nullptr);
    ctx->OMSetDepthStencilState(captureDepth_.Get(), 0);
    ctx->OMSetBlendState(overwrite_.Get(), nullptr, 0xffffffff);
    ctx->PSSetShader(reverse ? captureReversed_.Get() : captureShader_.Get(), nullptr, 0);
    switch (params.type) {
    case DrawCallType::Draw:
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT)>(original)(ctx,params.vertexCount,params.startVertexLocation); break;
    case DrawCallType::DrawIndexed:
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT)>(original)(ctx,params.indexCount,params.startIndexLocation,params.baseVertexLocation); break;
    case DrawCallType::DrawInstanced:
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT)>(original)(ctx,params.vertexCount,params.instanceCount,params.startVertexLocation,params.startInstanceLocation); break;
    case DrawCallType::DrawIndexedInstanced:
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT)>(original)(ctx,params.indexCount,params.instanceCount,params.startIndexLocation,params.baseVertexLocation,params.startInstanceLocation); break;
    }
    ctx->PSSetShader(pixel.Get(), instances, instanceCount);
    for (UINT i = 0; i < instanceCount; ++i) if (instances[i]) instances[i]->Release();
    ctx->OMSetBlendState(blend.Get(), factor, sampleMask);
    ctx->OMSetDepthStencilState(depthState.Get(), reference);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depthView.Get());
    for (auto* target : targets) if (target) target->Release();
    ctx->SetPredication(predicate.Get(), predicateValue);
    if (!pending) { if (localPlayer) ++localCaptures_; else ++captures_; reversed_ = reverse; }
    ds3scOutlineCaptureRejectReason = 0;
    return true;
}

void LiveAllyOutline::BeforeDepthClear(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* view,
                                      UINT flags, float depth) noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    if (!ctx || !(flags & D3D11_CLEAR_DEPTH) || !device_) return;
    ComPtr<ID3D11Texture2D> texture; D3D11_TEXTURE2D_DESC desc{};
    if (!TextureView(view, device_.Get(), texture, desc) ||
        desc.Width != width_ || desc.Height != height_) return;
    {
        Unpredicated unpredicated(ctx);
        if (ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) {
            // A clear-only list may execute after a different worker's ally list.
            // Its snapshot must therefore survive independently until execution.
            auto pending = Pending(ctx, generation_);
            if (pending) {
                auto previousCopy = sceneCopy_; auto previousView = sceneView_;
                sceneCopy_.Reset(); sceneView_.Reset();
                if (SUCCEEDED(PrepareSceneCopy(texture.Get()))) {
                    ctx->CopyResource(sceneCopy_.Get(), texture.Get());
                    CommandEvent event; event.source = texture; event.depth = sceneCopy_; event.view = sceneView_;
                    try { pending->events.push_back(std::move(event)); } catch (...) { /* Skip this snapshot on allocation failure. */ }
                }
                sceneCopy_ = previousCopy; sceneView_ = previousView;
            }
        } else if (scene_ == texture && (captures_ || localCaptures_) && !sceneFrozen_ && SUCCEEDED(PrepareSceneCopy(texture.Get()))) {
            ctx->CopyResource(sceneCopy_.Get(), texture.Get());
            sceneFrozen_ = true;
        }
    }
    for (auto& clear : depthClears_) if (clear.texture == texture) { clear.depth = depth; return; }
    depthClears_[nextClear_] = {texture, depth};
    nextClear_ = (nextClear_ + 1) % static_cast<UINT>(depthClears_.size());
}

void LiveAllyOutline::Finish(ID3D11DeviceContext* ctx, ID3D11CommandList* list) noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    auto pending = GetCommands(ctx);
    if (pending && list) list->SetPrivateDataInterface(kOutlineCommands, pending.Get());
    if (ctx) ctx->SetPrivateDataInterface(kOutlineCommands, nullptr);
}

bool LiveAllyOutline::Executed(ID3D11CommandList* list) noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    auto pending = GetCommands(list);
    if (!pending || pending->generation != generation_) return false;
    bool captured = false;
    for (const auto& event : pending->events) {
        if (event.count) {
            if (scene_ && scene_ != event.source) mixedScene_ = true;
            if (!scene_) {
                scene_ = event.source;
                reversed_ = event.reverse;
            } else if (event.reverse) {
                reversed_ = true;
            }
            if (event.localPlayer) localCaptures_ += event.count;
            else { captures_ += event.count; captured = true; }
        } else if (event.depth && scene_ == event.source && (captures_ || localCaptures_) && !sceneFrozen_) {
            sceneCopy_ = event.depth; sceneView_ = event.view; sceneFrozen_ = true;
        }
    }
    return captured;
}

void LiveAllyOutline::EndFrame() noexcept {
    lastCaptures_ = captures_;
    lastLocalCaptures_ = localCaptures_;
    captures_ = localCaptures_ = 0; sceneFrozen_ = mixedScene_ = false; scene_.Reset(); ++frame_;
    // Snapshots can be owned by reusable command lists: never overwrite them.
    sceneCopy_.Reset(); sceneView_.Reset();
}

void LiveAllyOutline::RenderFallbackMarkers(ID3D11DeviceContext* context, ID3D11RenderTargetView* targetRTV) noexcept {
    if (!context || !targetRTV || !markerVertexShader_ || !markerPixelShader_ || !markerConstantBuffer_) return;

    ActorTracker::AllyScreenProjection projections[8]{};
    const std::size_t projCount = ActorTracker::Instance().GetAllyProjections(
        projections,
        8,
        static_cast<float>(width_),
        static_cast<float>(height_)
    );
    if (projCount == 0) return;

    static float pulseTimer = 0.0f;
    pulseTimer += 0.035f;
    if (pulseTimer > 6.2831853f) pulseTimer -= 6.2831853f;
    const float pulseAlpha = 0.70f + 0.20f * std::sin(pulseTimer);

    MarkerConstants cb{};
    cb.markerCount = static_cast<UINT>(projCount);
    cb.hasLocalMask = (localView_ && localCaptures_ > 0) ? 1 : 0;
    // AshWhite: 0.871f, 0.847f, 0.791f
    cb.markerColor[0] = 0.871f;
    cb.markerColor[1] = 0.847f;
    cb.markerColor[2] = 0.791f;
    cb.markerColor[3] = pulseAlpha;

    for (std::size_t i = 0; i < projCount; ++i) {
        cb.markerPos[i][0] = projections[i].screenX;
        cb.markerPos[i][1] = projections[i].screenY;
        cb.markerPos[i][2] = projections[i].distance;
        cb.markerPos[i][3] = 1.0f;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(markerConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, &cb, sizeof(MarkerConstants));
    context->Unmap(markerConstantBuffer_.Get(), 0);

    D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(markerRaster_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(markerVertexShader_.Get(), nullptr, 0);
    context->PSSetShader(markerPixelShader_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { markerConstantBuffer_.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);
    if (cb.hasLocalMask && localView_) {
        ID3D11ShaderResourceView* srvs[] = { localView_.Get() };
        context->PSSetShaderResources(0, 1, srvs);
    }
    ID3D11RenderTargetView* rtvs[] = { targetRTV };
    context->OMSetRenderTargets(1, rtvs, nullptr);
    context->OMSetBlendState(markerBlendState_.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(markerDepthState_.Get(), 0);

    context->Draw(3, 0);

    if (cb.hasLocalMask) {
        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        context->PSSetShaderResources(0, 1, nullSrv);
    }
}

HRESULT LiveAllyOutline::Present(IDXGISwapChain* swap, bool enabled, bool showMask, float thickness, bool visibleOutline, bool fillSilhouette, float fillOpacity, bool fallbackMarkers, bool playerOutline) noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    struct FrameGuard { LiveAllyOutline* self; ~FrameGuard() { self->EndFrame(); } } guard{this};
    if (!swap) return E_INVALIDARG;
    ComPtr<ID3D11Device> device; HRESULT hr = swap->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(hr = swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return hr;
    D3D11_TEXTURE2D_DESC desc{}; backbuffer->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1) return E_INVALIDARG;
    if (device_ != device || width_ != desc.Width || height_ != desc.Height)
        return Initialize(device.Get(), desc.Width, desc.Height);
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    Unpredicated unpredicated(context.Get());
    hr = S_FALSE;

    if (playerOutline && localCaptures_ && localView_) {
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view.Format = desc.Format;
        if (view.Format == DXGI_FORMAT_R8G8B8A8_UNORM) view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (view.Format == DXGI_FORMAT_B8G8R8A8_UNORM) view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        ComPtr<ID3D11RenderTargetView> target;
        hr = device->CreateRenderTargetView(backbuffer.Get(), &view, &target);
        if (FAILED(hr)) {
            hr = device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target);
        }
        if (SUCCEEDED(hr)) {
            hr = RenderPlayerDiagnostic(context.Get(), target.Get(), true, thickness);
        }
    }

    if (enabled && !mixedScene_ && captures_ && scene_ && SUCCEEDED(PrepareSceneCopy(scene_.Get()))) {
        if (!sceneFrozen_) context->CopyResource(sceneCopy_.Get(), scene_.Get());
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view.Format = desc.Format;
        if (view.Format == DXGI_FORMAT_R8G8B8A8_UNORM) view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (view.Format == DXGI_FORMAT_B8G8R8A8_UNORM) view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        ComPtr<ID3D11RenderTargetView> target;
        hr = device->CreateRenderTargetView(backbuffer.Get(), &view, &target);
        if (FAILED(hr)) {
            hr = device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target);
        }
        if (SUCCEEDED(hr)) {
            OutlineFrame frame{maskView_.Get(), sceneView_.Get(), target.Get(), frame_, frame_, frame_,
                true, true, reversed_, false, visibleOutline, showMask, thickness, fillSilhouette, fillOpacity, true,
                playerOutline ? nullptr : (localCaptures_ ? localView_.Get() : nullptr)};
            hr = outline_.Draw(context.Get(), frame);
        }
    } else if (enabled && fallbackMarkers && captures_ == 0 && ActorTracker::Instance().HasAllies() && !ActorTracker::Instance().IsGameMenuOpen()) {
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view.Format = desc.Format;
        if (view.Format == DXGI_FORMAT_R8G8B8A8_UNORM) view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (view.Format == DXGI_FORMAT_B8G8R8A8_UNORM) view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        ComPtr<ID3D11RenderTargetView> target;
        hr = device->CreateRenderTargetView(backbuffer.Get(), &view, &target);
        if (FAILED(hr)) {
            hr = device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target);
        }
        if (SUCCEEDED(hr)) {
            RenderFallbackMarkers(context.Get(), target.Get());
            hr = S_OK;
        }
    }
    const float clear[4]{}; context->ClearRenderTargetView(maskTarget_.Get(), clear);
    context->ClearRenderTargetView(localTarget_.Get(), clear);
    ds3scOutlineComposeResult = hr;
    return hr;
}


} // namespace ds3sc::render
