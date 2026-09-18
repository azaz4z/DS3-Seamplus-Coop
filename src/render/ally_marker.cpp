#include "ally_marker.h"
#include "actor_tracker.h"
#if (defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE) || (defined(DS3SC_FEATURE_PLAYER_OUTLINE) && DS3SC_FEATURE_PLAYER_OUTLINE)
#include "ally_outline.h"
#endif
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace ds3sc::render {

namespace {

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

struct Unpredicated {
    ID3D11DeviceContext* context;
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;
    BOOL value = FALSE;
    explicit Unpredicated(ID3D11DeviceContext* ctx) : context(ctx) {
        if (context) {
            context->GetPredication(&predicate, &value);
            context->SetPredication(nullptr, FALSE);
        }
    }
    ~Unpredicated() {
        if (context) {
            context->SetPredication(predicate.Get(), value);
        }
    }
};

} // namespace

LiveAllyMarkers& LiveAllyMarkers::Instance() noexcept {
    static LiveAllyMarkers instance;
    return instance;
}

void LiveAllyMarkers::Reset() noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    device_.Reset();
    markerVertexShader_.Reset();
    markerPixelShader_.Reset();
    markerConstantBuffer_.Reset();
    markerBlendState_.Reset();
    markerDepthState_.Reset();
    markerRaster_.Reset();
    width_ = 0;
    height_ = 0;
}

HRESULT LiveAllyMarkers::Initialize(ID3D11Device* device, UINT width, UINT height) noexcept {
    std::lock_guard<std::recursive_mutex> lock(renderMutex_);
    if (!device || width == 0 || height == 0) return E_INVALIDARG;

    if (device_.Get() == device && width_ == width && height_ == height && markerVertexShader_) {
        return S_OK;
    }

    device_ = device;
    width_ = width;
    height_ = height;

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(kMarkerVS, strlen(kMarkerVS), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
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

    return S_OK;
}

HRESULT LiveAllyMarkers::Present(IDXGISwapChain* swap, bool enabled) noexcept {
    if (!enabled || !swap) return S_FALSE;
    if (ActorTracker::Instance().IsGameMenuOpen()) return S_FALSE;
    if (!ActorTracker::Instance().HasAllies()) return S_FALSE;

    std::lock_guard<std::recursive_mutex> lock(renderMutex_);

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    HRESULT hr = swap->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) return hr;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
    hr = swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr) || !backbuffer) return hr;

    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1) return E_INVALIDARG;

    if (device_.Get() != device.Get() || width_ != desc.Width || height_ != desc.Height) {
        hr = Initialize(device.Get(), desc.Width, desc.Height);
        if (FAILED(hr)) return hr;
    }

    if (!markerVertexShader_ || !markerPixelShader_ || !markerConstantBuffer_) {
        return E_FAIL;
    }

    ID3D11ShaderResourceView* localView = nullptr;
    bool hasLocal = false;
#if (defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE) || (defined(DS3SC_FEATURE_PLAYER_OUTLINE) && DS3SC_FEATURE_PLAYER_OUTLINE)
    LiveAllyOutline::Instance().EnsureInitialized(device.Get(), desc.Width, desc.Height);
    localView = LiveAllyOutline::Instance().GetLocalMaskView();
    hasLocal = (localView && LiveAllyOutline::Instance().GetLocalCaptures() > 0);
#endif

    ActorTracker::AllyScreenProjection projections[8]{};
    const std::size_t projCount = ActorTracker::Instance().GetAllyProjections(
        projections,
        8,
        static_cast<float>(width_),
        static_cast<float>(height_),
        !hasLocal
    );
    if (projCount == 0) return S_OK;

    pulseTimer_ += 0.035f;
    if (pulseTimer_ > 6.2831853f) pulseTimer_ -= 6.2831853f;
    const float pulseAlpha = 0.70f + 0.20f * std::sin(pulseTimer_);

    MarkerConstants cb{};
    cb.markerCount = static_cast<UINT>(projCount);
    cb.hasLocalMask = hasLocal ? 1 : 0;
    // DS3 Ash White
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

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return E_FAIL;

    // Occlusion predication by DS3 silently discards draw calls if actors are occluded by walls.
    // Unpredicated disables predication for our HUD overlay.
    Unpredicated unpredicated(context.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(markerConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return E_FAIL;
    }
    std::memcpy(mapped.pData, &cb, sizeof(MarkerConstants));
    context->Unmap(markerConstantBuffer_.Get(), 0);

    // Create fresh RTV for current swapchain backbuffer
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    view.Format = desc.Format;
    if (view.Format == DXGI_FORMAT_R8G8B8A8_UNORM) view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (view.Format == DXGI_FORMAT_B8G8R8A8_UNORM) view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> targetRtv;
    hr = device->CreateRenderTargetView(backbuffer.Get(), &view, &targetRtv);
    if (FAILED(hr)) {
        hr = device->CreateRenderTargetView(backbuffer.Get(), nullptr, &targetRtv);
    }
    if (FAILED(hr) || !targetRtv) return hr;

    // Preserve D3D11 state
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> prevRtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> prevDsv;
    context->OMGetRenderTargets(1, &prevRtv, &prevDsv);

    Microsoft::WRL::ComPtr<ID3D11BlendState> prevBlend;
    FLOAT prevBlendFactor[4]{};
    UINT prevSampleMask = 0;
    context->OMGetBlendState(&prevBlend, prevBlendFactor, &prevSampleMask);

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> prevDepth;
    UINT prevStencilRef = 0;
    context->OMGetDepthStencilState(&prevDepth, &prevStencilRef);

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> prevRaster;
    context->RSGetState(&prevRaster);

    UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&numViewports, prevViewports);

    Microsoft::WRL::ComPtr<ID3D11VertexShader> prevVs;
    context->VSGetShader(&prevVs, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11PixelShader> prevPs;
    context->PSGetShader(&prevPs, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11Buffer> prevVsCb;
    context->VSGetConstantBuffers(0, 1, &prevVsCb);

    Microsoft::WRL::ComPtr<ID3D11Buffer> prevPsCb;
    context->PSGetConstantBuffers(0, 1, &prevPsCb);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prevPsSrv;
    context->PSGetShaderResources(0, 1, &prevPsSrv);

    D3D11_PRIMITIVE_TOPOLOGY prevTopology;
    context->IAGetPrimitiveTopology(&prevTopology);

    // Apply diamond marker draw state
    D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);
    context->RSSetState(markerRaster_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(markerVertexShader_.Get(), nullptr, 0);
    context->PSSetShader(markerPixelShader_.Get(), nullptr, 0);

    ID3D11Buffer* cbs[] = { markerConstantBuffer_.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);

    if (cb.hasLocalMask != 0 && localView) {
        ID3D11ShaderResourceView* srvs[] = { localView };
        context->PSSetShaderResources(0, 1, srvs);
    }

    ID3D11RenderTargetView* rtvs[] = { targetRtv.Get() };
    context->OMSetRenderTargets(1, rtvs, nullptr);
    context->OMSetBlendState(markerBlendState_.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(markerDepthState_.Get(), 0);

    context->Draw(3, 0);

    if (cb.hasLocalMask != 0) {
        ID3D11ShaderResourceView* nullSrvs[] = { nullptr };
        context->PSSetShaderResources(0, 1, nullSrvs);
    }

    // Restore state
    context->RSSetViewports(numViewports, prevViewports);
    context->RSSetState(prevRaster.Get());
    context->IASetPrimitiveTopology(prevTopology);
    context->VSSetShader(prevVs.Get(), nullptr, 0);
    context->PSSetShader(prevPs.Get(), nullptr, 0);
    ID3D11Buffer* vsCbs[] = { prevVsCb.Get() };
    context->VSSetConstantBuffers(0, 1, vsCbs);
    ID3D11Buffer* psCbs[] = { prevPsCb.Get() };
    context->PSSetConstantBuffers(0, 1, psCbs);
    ID3D11ShaderResourceView* prevSrvs[] = { prevPsSrv.Get() };
    context->PSSetShaderResources(0, 1, prevSrvs);
    ID3D11RenderTargetView* prevRtvs[] = { prevRtv.Get() };
    context->OMSetRenderTargets(1, prevRtvs, prevDsv.Get());
    context->OMSetBlendState(prevBlend.Get(), prevBlendFactor, prevSampleMask);
    context->OMSetDepthStencilState(prevDepth.Get(), prevStencilRef);

    return S_OK;
}

} // namespace ds3sc::render
