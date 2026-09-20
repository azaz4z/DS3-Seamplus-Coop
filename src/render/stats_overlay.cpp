#include "stats_overlay.h"
#include "../extensions/counters/counters_extension.h"

#include <d3dcompiler.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cmath>

namespace ds3sc::render {
namespace {

struct Vertex {
    float x, y;       // NDC [-1, 1]
    float u, v;       // UV [0, 1]
    float r, g, b, a; // Color
    float mode;       // 0 = solid, 1 = font
};

struct ConstantBufferData {
    float viewportWidth;
    float viewportHeight;
    float padding[2];
};

// Classic monochrome 8x8 bitmap font for ASCII characters 32 to 126 (95 characters)
// Each row represents 1 byte (8 bits = 8 pixels)
alignas(16) const unsigned char kFont8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' (32)
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
    {0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00}, // '''
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, // '0'
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1'
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, // '2'
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // '3'
    {0x0C,0x1C,0x34,0x64,0x7E,0x04,0x04,0x00}, // '4'
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // '5'
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, // '6'
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, // '7'
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // '8'
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, // '9'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // ':'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ';'
    {0x0E,0x1C,0x38,0x70,0x38,0x1C,0x0E,0x00}, // '<'
    {0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00}, // '='
    {0x70,0x38,0x1C,0x0E,0x1C,0x38,0x70,0x00}, // '>'
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}, // '?'
    {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00}, // '@'
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, // 'A'
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // 'B'
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // 'C'
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // 'D'
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, // 'E'
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, // 'F'
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3A,0x00}, // 'G'
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 'H'
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'I'
    {0x0E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 'J'
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // 'K'
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // 'L'
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // 'M'
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // 'N'
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'O'
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // 'P'
    {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, // 'Q'
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // 'R'
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // 'S'
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 'T'
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'U'
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'W'
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // 'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // 'Y'
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // 'Z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // '\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, // '_'
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, // 'a'
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // 'b'
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, // 'c'
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, // 'd'
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, // 'e'
    {0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00}, // 'f'
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, // 'g'
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, // 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i'
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x3C,0x00}, // 'j'
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, // 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l'
    {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, // 'm'
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, // 'n'
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, // 'o'
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, // 'p'
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x07}, // 'q'
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, // 'r'
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, // 's'
    {0x18,0x18,0x7E,0x18,0x18,0x18,0x0C,0x00}, // 't'
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, // 'u'
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, // 'v'
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 'w'
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, // 'x'
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, // 'y'
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, // 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // '~' (126)
};

const char* kShadersHlsl = R"(
cbuffer ViewportBuffer : register(b0) {
    float viewportWidth;
    float viewportHeight;
    float2 padding;
};

struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
    float mode : BLENDINDICES0;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
    float mode : BLENDINDICES0;
};

Texture2D fontTexture : register(t0);
SamplerState fontSampler : register(s0);

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    // Screen coordinate (pixels) to NDC [-1, 1] conversion
    float x = (input.pos.x / viewportWidth) * 2.0f - 1.0f;
    float y = 1.0f - (input.pos.y / viewportHeight) * 2.0f;
    output.pos = float4(x, y, 0.0f, 1.0f);
    output.uv = input.uv;
    output.col = input.col;
    output.mode = input.mode;
    return output;
}

float4 PSMain(PS_INPUT input) : SV_Target {
    if (input.mode > 0.5f) {
        float alpha = fontTexture.Sample(fontSampler, input.uv).r;
        if (alpha < 0.05f) discard;
        return float4(input.col.rgb, input.col.a * alpha);
    }
    return input.col;
}
)";

static bool IsSpanishLanguage() noexcept {
    static int cached = -1;
    if (cached != -1) return cached == 1;

    char iniPath[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&IsSpanishLanguage), &hMod) && hMod) {
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

    char langBuf[64] = {};
    GetPrivateProfileStringA("LANGUAGE", "mod_language_override", "", langBuf, sizeof(langBuf), iniPath);
    if (_stricmp(langBuf, "spanish") == 0 || _stricmp(langBuf, "es") == 0) {
        cached = 1;
        return true;
    }
    if (_stricmp(langBuf, "english") == 0 || _stricmp(langBuf, "en") == 0) {
        cached = 0;
        return false;
    }

    LANGID langId = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(langId) == LANG_SPANISH) {
        cached = 1;
        return true;
    }

    cached = 0;
    return false;
}

} // namespace


CombatStatsOverlay& CombatStatsOverlay::Instance() noexcept {
    static CombatStatsOverlay s_instance;
    return s_instance;
}

void CombatStatsOverlay::Reset() noexcept {
    vertexShader_.Reset();
    pixelShader_.Reset();
    vertexBuffer_.Reset();
    constantBuffer_.Reset();
    inputLayout_.Reset();
    fontTexture_.Reset();
    fontSRV_.Reset();
    fontSampler_.Reset();
    blendState_.Reset();
    depthState_.Reset();
    rasterState_.Reset();
    rtv_.Reset();
    device_.Reset();
    width_ = 0;
    height_ = 0;

}

HRESULT CombatStatsOverlay::EnsureResources(ID3D11Device* device, UINT width, UINT height) noexcept {
    if (device_.Get() == device && width_ == width && height_ == height && vertexBuffer_) {
        return S_OK;
    }

    Reset();
    device_ = device;
    width_ = width;
    height_ = height;

    // 1. Compile Shaders
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    HRESULT hr = D3DCompile(kShadersHlsl, std::strlen(kShadersHlsl), "CombatStatsOverlay", nullptr, nullptr,
                            "VSMain", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        return hr;
    }

    hr = D3DCompile(kShadersHlsl, std::strlen(kShadersHlsl), "CombatStatsOverlay", nullptr, nullptr,
                    "PSMain", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        return hr;
    }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader_);
    if (FAILED(hr)) return hr;

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader_);
    if (FAILED(hr)) return hr;

    // 2. Input Layout
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R32_FLOAT,          0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(layoutDesc, 4, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_);
    if (FAILED(hr)) return hr;

    // 3. Dynamic Vertex Buffer (up to 3000 vertices = 500 quads)
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(Vertex) * 3000;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&vbDesc, nullptr, &vertexBuffer_);
    if (FAILED(hr)) return hr;

    // 4. Constant Buffer (viewport dimensions)
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(ConstantBufferData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, nullptr, &constantBuffer_);
    if (FAILED(hr)) return hr;

    // 5. Create texture atlas for 8x8 bitmap font (128x64 pixels, 16 columns x 8 rows of characters)
    constexpr UINT texW = 128;
    constexpr UINT texH = 64;
    std::vector<unsigned char> fontPixels(texW * texH, 0);

    for (int ch = 0; ch < 95; ++ch) {
        const int tileX = (ch % 16) * 8;
        const int tileY = (ch / 16) * 8;
        for (int row = 0; row < 8; ++row) {
            const unsigned char bits = kFont8x8[ch][row];
            for (int col = 0; col < 8; ++col) {
                if ((bits >> (7 - col)) & 1) {
                    fontPixels[(tileY + row) * texW + (tileX + col)] = 255;
                }
            }
        }
    }

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = texW;
    texDesc.Height = texH;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subData{};
    subData.pSysMem = fontPixels.data();
    subData.SysMemPitch = texW;

    hr = device->CreateTexture2D(&texDesc, &subData, &fontTexture_);
    if (FAILED(hr)) return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(fontTexture_.Get(), &srvDesc, &fontSRV_);
    if (FAILED(hr)) return hr;

    // 6. Sampler State (Point Clamp for pixel-perfect text)
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device->CreateSamplerState(&sampDesc, &fontSampler_);
    if (FAILED(hr)) return hr;

    // 7. Blend State (Traditional alpha blending)
    D3D11_BLEND_DESC bDesc{};
    bDesc.RenderTarget[0].BlendEnable = TRUE;
    bDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bDesc, &blendState_);
    if (FAILED(hr)) return hr;

    // 8. Depth Stencil State (Depth disabled to draw overlay directly on top)
    D3D11_DEPTH_STENCIL_DESC dDesc{};
    dDesc.DepthEnable = FALSE;
    dDesc.StencilEnable = FALSE;
    hr = device->CreateDepthStencilState(&dDesc, &depthState_);
    if (FAILED(hr)) return hr;

    // 9. Rasterizer State (Cull None)
    D3D11_RASTERIZER_DESC rDesc{};
    rDesc.FillMode = D3D11_FILL_SOLID;
    rDesc.CullMode = D3D11_CULL_NONE;
    rDesc.DepthClipEnable = FALSE;
    hr = device->CreateRasterizerState(&rDesc, &rasterState_);
    return hr;
}

HRESULT CombatStatsOverlay::Present(IDXGISwapChain* swapChain) noexcept {
    if (!swapChain) return S_OK;

    auto* ext = extensions::GetCountersInstance();
    if (!ext || !ext->IsOverlayVisible()) {
        return S_OK;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        return S_OK;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) {
        return S_OK;
    }

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);

    if (FAILED(EnsureResources(device.Get(), bbDesc.Width, bbDesc.Height))) {
        return S_OK;
    }

    if (!rtv_) {
        if (FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_))) {
            return S_OK;
        }
    }


    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx) return S_OK;

    // Save full D3D11 pipeline state to avoid interfering with the game
    D3D11_VIEWPORT savedViewport{};
    UINT numViewports = 1;
    ctx->RSGetViewports(&numViewports, &savedViewport);

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> savedRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> savedDSV;
    ctx->OMGetRenderTargets(1, &savedRTV, &savedDSV);

    Microsoft::WRL::ComPtr<ID3D11BlendState> savedBlend;
    FLOAT savedBlendFactor[4]{};
    UINT savedSampleMask = 0;
    ctx->OMGetBlendState(&savedBlend, savedBlendFactor, &savedSampleMask);

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> savedDepth;
    UINT savedStencilRef = 0;
    ctx->OMGetDepthStencilState(&savedDepth, &savedStencilRef);

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> savedRaster;
    ctx->RSGetState(&savedRaster);

    Microsoft::WRL::ComPtr<ID3D11VertexShader> savedVS;
    ctx->VSGetShader(&savedVS, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11PixelShader> savedPS;
    ctx->PSGetShader(&savedPS, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11InputLayout> savedLayout;
    ctx->IAGetInputLayout(&savedLayout);

    D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
    ctx->IAGetPrimitiveTopology(&savedTopology);

    Microsoft::WRL::ComPtr<ID3D11Buffer> savedVB;
    UINT savedStride = 0, savedOffset = 0;
    ctx->IAGetVertexBuffers(0, 1, &savedVB, &savedStride, &savedOffset);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> savedSRV;
    ctx->PSGetShaderResources(0, 1, &savedSRV);

    Microsoft::WRL::ComPtr<ID3D11SamplerState> savedSampler;
    ctx->PSGetSamplers(0, 1, &savedSampler);

    Microsoft::WRL::ComPtr<ID3D11Buffer> savedCB;
    ctx->VSGetConstantBuffers(0, 1, &savedCB);

    // Prepare vertex list for HUD
    std::vector<Vertex> vertices;
    vertices.reserve(1500);

    const auto stats = ext->GetStats();
    const float startX = static_cast<float>(ext->GetOverlayX());
    const float startY = static_cast<float>(ext->GetOverlayY());

    // Helper function to add a solid rectangle
    auto addRect = [&](float rx, float ry, float rw, float rh, float cr, float cg, float cb, float ca) {
        Vertex v0{ rx,      ry,      0.0f, 0.0f, cr, cg, cb, ca, 0.0f };
        Vertex v1{ rx + rw, ry,      0.0f, 0.0f, cr, cg, cb, ca, 0.0f };
        Vertex v2{ rx,      ry + rh, 0.0f, 0.0f, cr, cg, cb, ca, 0.0f };
        Vertex v3{ rx + rw, ry + rh, 0.0f, 0.0f, cr, cg, cb, ca, 0.0f };
        // Triangle 1
        vertices.push_back(v0);
        vertices.push_back(v1);
        vertices.push_back(v2);
        // Triangle 2
        vertices.push_back(v1);
        vertices.push_back(v3);
        vertices.push_back(v2);
    };

    // HUD box dimensions
    constexpr float boxW = 320.0f;
    constexpr float boxH = 175.0f;
    constexpr float borderW = 2.0f;

    // 1. Dark translucent background (Dark Souls charcoal: #0f0f11 @ 88%)
    addRect(startX, startY, boxW, boxH, 0.06f, 0.06f, 0.07f, 0.88f);

    // 2. Outer ashen gold border (#a8946e)
    addRect(startX, startY, boxW, borderW, 0.66f, 0.58f, 0.43f, 0.95f); // Top border
    addRect(startX, startY + boxH - borderW, boxW, borderW, 0.66f, 0.58f, 0.43f, 0.95f); // Bottom border
    addRect(startX, startY, borderW, boxH, 0.66f, 0.58f, 0.43f, 0.95f); // Left border
    addRect(startX + boxW - borderW, startY, borderW, boxH, 0.66f, 0.58f, 0.43f, 0.95f); // Right border

    // 3. Dividing line under title
    addRect(startX + 8.0f, startY + 34.0f, boxW - 16.0f, 1.0f, 0.50f, 0.44f, 0.32f, 0.70f);

    // 4. Dividing line above help keys
    addRect(startX + 8.0f, startY + 148.0f, boxW - 16.0f, 1.0f, 0.40f, 0.38f, 0.35f, 0.50f);

    // Helper function to add text
    auto addText = [&](float tx, float ty, const char* str, float scaleX, float scaleY,
                       float cr, float cg, float cb, float ca) {
        if (!str) return;
        float curX = tx;
        for (const char* p = str; *p; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c < 32 || c > 126) c = '?';
            const int ch = c - 32;
            const float u0 = static_cast<float>((ch % 16) * 8) / 128.0f;
            const float v0 = static_cast<float>((ch / 16) * 8) / 64.0f;
            const float u1 = u0 + (8.0f / 128.0f);
            const float v1 = v0 + (8.0f / 64.0f);

            const float charW = 8.0f * scaleX;
            const float charH = 8.0f * scaleY;

            Vertex v0_{ curX,         ty,         u0, v0, cr, cg, cb, ca, 1.0f };
            Vertex v1_{ curX + charW, ty,         u1, v0, cr, cg, cb, ca, 1.0f };
            Vertex v2_{ curX,         ty + charH, u0, v1, cr, cg, cb, ca, 1.0f };
            Vertex v3_{ curX + charW, ty + charH, u1, v1, cr, cg, cb, ca, 1.0f };

            vertices.push_back(v0_);
            vertices.push_back(v1_);
            vertices.push_back(v2_);
            vertices.push_back(v1_);
            vertices.push_back(v3_);
            vertices.push_back(v2_);

            curX += charW + 1.0f; // Character spacing
        }
    };

    // Render header
    addText(startX + 16.0f, startY + 12.0f, "COUNTERS", 1.35f, 1.45f, 0.88f, 0.78f, 0.55f, 1.0f);

    // Render stats rows
    char valBuf[32];
    const float labelX = startX + 16.0f;
    const float valX = startX + 240.0f;
    float rowY = startY + 44.0f;
    constexpr float rowGap = 24.0f;

    // 1. Deaths
    addText(labelX, rowY, "Deaths:", 1.15f, 1.35f, 0.82f, 0.82f, 0.82f, 1.0f);
    std::snprintf(valBuf, sizeof(valBuf), "%u", stats.deaths);
    addText(valX, rowY, valBuf, 1.25f, 1.45f, 0.95f, 0.35f, 0.35f, 1.0f); // Soft red

    // 2. Enemy Kills
    rowY += rowGap;
    addText(labelX, rowY, "Enemy Kills:", 1.15f, 1.35f, 0.82f, 0.82f, 0.82f, 1.0f);
    std::snprintf(valBuf, sizeof(valBuf), "%u", stats.kills);
    addText(valX, rowY, valBuf, 1.25f, 1.45f, 0.45f, 0.85f, 0.45f, 1.0f); // Soft green

    // 3. Backstabs Dealt
    rowY += rowGap;
    addText(labelX, rowY, "Backstabs Dealt:", 1.15f, 1.35f, 0.82f, 0.82f, 0.82f, 1.0f);
    std::snprintf(valBuf, sizeof(valBuf), "%u", stats.backstabsInflicted);
    addText(valX, rowY, valBuf, 1.25f, 1.45f, 0.95f, 0.82f, 0.45f, 1.0f); // Bright gold

    // 4. Backstabs Taken
    rowY += rowGap;
    addText(labelX, rowY, "Backstabs Taken:", 1.15f, 1.35f, 0.82f, 0.82f, 0.82f, 1.0f);
    std::snprintf(valBuf, sizeof(valBuf), "%u", stats.backstabsReceived);
    addText(valX, rowY, valBuf, 1.25f, 1.45f, 0.88f, 0.55f, 0.30f, 1.0f); // Soft orange

    // Footer help / shortcuts
    addText(startX + 14.0f, startY + 155.0f, "[F8] Toggle HUD   [F9] Reset", 0.95f, 1.1f, 0.55f, 0.55f, 0.58f, 0.90f);

    // Map vertex buffer and upload data
    if (!vertices.empty()) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const UINT copyCount = (vertices.size() < 3000) ? static_cast<UINT>(vertices.size()) : 3000;
            std::memcpy(mapped.pData, vertices.data(), sizeof(Vertex) * copyCount);
            ctx->Unmap(vertexBuffer_.Get(), 0);

            // Upload viewport constants
            if (SUCCEEDED(ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                auto* cb = static_cast<ConstantBufferData*>(mapped.pData);
                cb->viewportWidth = static_cast<float>(bbDesc.Width);
                cb->viewportHeight = static_cast<float>(bbDesc.Height);
                ctx->Unmap(constantBuffer_.Get(), 0);
            }

            // Set full viewport
            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<FLOAT>(bbDesc.Width);
            vp.Height = static_cast<FLOAT>(bbDesc.Height);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);

            // Set backbuffer RTV
            ID3D11RenderTargetView* targets[1] = { rtv_.Get() };
            ctx->OMSetRenderTargets(1, targets, nullptr);


            // Configure states
            FLOAT blendFactors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            ctx->OMSetBlendState(blendState_.Get(), blendFactors, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(depthState_.Get(), 0);
            ctx->RSSetState(rasterState_.Get());

            // Configure shaders and buffers
            ctx->VSSetShader(vertexShader_.Get(), nullptr, 0);
            ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
            ctx->VSSetConstantBuffers(0, 1, cbs);

            ctx->PSSetShader(pixelShader_.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvs[1] = { fontSRV_.Get() };
            ctx->PSSetShaderResources(0, 1, srvs);
            ID3D11SamplerState* samps[1] = { fontSampler_.Get() };
            ctx->PSSetSamplers(0, 1, samps);

            ctx->IASetInputLayout(inputLayout_.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            UINT stride = sizeof(Vertex);
            UINT offset = 0;
            ID3D11Buffer* vbs[1] = { vertexBuffer_.Get() };
            ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);

            // Draw entire HUD
            ctx->Draw(copyCount, 0);
        }
    }

    // Restore previous DirectX 11 state
    ctx->RSSetViewports(numViewports, &savedViewport);
    ID3D11RenderTargetView* prevRTVs[1] = { savedRTV.Get() };
    ctx->OMSetRenderTargets(1, prevRTVs, savedDSV.Get());
    ctx->OMSetBlendState(savedBlend.Get(), savedBlendFactor, savedSampleMask);
    ctx->OMSetDepthStencilState(savedDepth.Get(), savedStencilRef);
    ctx->RSSetState(savedRaster.Get());
    ctx->VSSetShader(savedVS.Get(), nullptr, 0);
    ctx->PSSetShader(savedPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(savedLayout.Get());
    ctx->IASetPrimitiveTopology(savedTopology);
    ID3D11Buffer* prevVBs[1] = { savedVB.Get() };
    ctx->IASetVertexBuffers(0, 1, prevVBs, &savedStride, &savedOffset);
    ID3D11ShaderResourceView* prevSRVs[1] = { savedSRV.Get() };
    ctx->PSSetShaderResources(0, 1, prevSRVs);
    ID3D11SamplerState* prevSamps[1] = { savedSampler.Get() };
    ctx->PSSetSamplers(0, 1, prevSamps);
    ID3D11Buffer* prevCBs[1] = { savedCB.Get() };
    ctx->VSSetConstantBuffers(0, 1, prevCBs);

    return S_OK;
}

} // namespace ds3sc::render
