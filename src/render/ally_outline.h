#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>
#include <mutex>

namespace ds3sc::render {

struct OutlineSettings {
    bool showAllyMask = false;       // Display raw mask on screen
    bool showAllyOutline = true;     // Enable outline composition
    bool fillSilhouette = true;      // Fill occluded inner silhouette
    bool wireframeContour = true;    // Defined polygonal wireframe contour
    bool showFallbackMarkers = true; // Fallback diamond markers
    float thickness = 2.0f;          // Thickness in pixels
    float opacity = 0.75f;           // Maximum outline opacity
    float fillOpacity = 0.40f;       // Inner fill opacity
    float outlineColor[3] = { 0.871f, 0.847f, 0.791f }; // DS3 ash white
};


enum class DrawCallType {
    Draw,
    DrawIndexed,
    DrawInstanced,
    DrawIndexedInstanced
};

struct ReplayDrawParams {
    DrawCallType type;
    UINT indexCount = 0;
    UINT startIndexLocation = 0;
    INT baseVertexLocation = 0;
    UINT vertexCount = 0;
    UINT startVertexLocation = 0;
    UINT instanceCount = 0;
    UINT startInstanceLocation = 0;
};

class AllyOutlineRenderer final {
public:
    static AllyOutlineRenderer& Instance() noexcept;

    // Initializes or reconfigures resources for SwapChain size
    HRESULT EnsureResources(ID3D11Device* device, UINT width, UINT height) noexcept;

    // Initializes states for occluded rendering behind walls
    HRESULT EnsureOccludedStates(ID3D11Device* device) noexcept;

    // Releases resolution-dependent resources (for ResizeBuffers)
    void ReleaseResolutionResources() noexcept;

    // Cleans and releases all COM resources
    void Reset() noexcept;

    // Draws ally submesh exclusively when occluded behind walls
    void DrawOccludedMesh(
        ID3D11DeviceContext* ctx,
        const ReplayDrawParams& params,
        void* originalDrawFn
    ) noexcept;

    // Replays an ally draw call onto allyMask texture
    void ReplayAllyDraw(
        ID3D11DeviceContext* ctx,
        const ReplayDrawParams& params,
        void* originalDrawFn
    ) noexcept;

    // Composes outline over Backbuffer in Present
    void ComposeOutline(
        ID3D11DeviceContext* ctx,
        IDXGISwapChain* swapChain
    ) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return device_ != nullptr; }
    [[nodiscard]] bool IsActive() const noexcept { return settings_.showAllyOutline || settings_.showAllyMask; }
    [[nodiscard]] OutlineSettings& Settings() noexcept { return settings_; }
    [[nodiscard]] const OutlineSettings& Settings() const noexcept { return settings_; }
    [[nodiscard]] UINT Width() const noexcept { return width_; }
    [[nodiscard]] UINT Height() const noexcept { return height_; }
    [[nodiscard]] UINT AllyDrawsThisFrame() const noexcept { return allyDrawsThisFrame_; }

    void ClearMask(ID3D11DeviceContext* ctx) noexcept;

private:
    AllyOutlineRenderer() = default;

    HRESULT CompileShaders(ID3D11Device* device) noexcept;
    HRESULT CreateStateObjects(ID3D11Device* device) noexcept;

    OutlineSettings settings_{};
    UINT width_ = 0;
    UINT height_ = 0;
    UINT allyDrawsThisFrame_ = 0;
    std::uint64_t frameCount_ = 0;
    std::uint64_t lastLogTime_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;

    // allyMask texture (DXGI_FORMAT_R8_UNORM)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> maskTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> maskRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskSRV_;

    // White mask and outline shaders
    Microsoft::WRL::ComPtr<ID3D11PixelShader> maskPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> outlineVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> outlinePixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> outlineConstantBuffer_;

    // Shaders and states for rendering behind walls
    Microsoft::WRL::ComPtr<ID3D11PixelShader> occludedPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> occludedWirePixelShader_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> occludedDepthStateStd_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> occludedDepthStateRev_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> occludedBlendState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> occludedRasterizerSolid_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> occludedRasterizerWire_;

    // General rendering states
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> maskDepthState_;   // DepthEnable = FALSE
    Microsoft::WRL::ComPtr<ID3D11BlendState> maskBlendState_;          // Overwrite
    Microsoft::WRL::ComPtr<ID3D11BlendState> outlineBlendState_;       // Alpha blending
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> outlineDepthState_;// Depth disabled
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;    // Cull None
    Microsoft::WRL::ComPtr<ID3D11SamplerState> maskSampler_;           // Clamp
};

struct OutlineFrame {
    ID3D11ShaderResourceView* allyMask = nullptr;
    ID3D11ShaderResourceView* sceneDepth = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    std::uint64_t frame = 0;
    std::uint64_t maskFrame = 0;
    std::uint64_t depthFrame = 0;
    bool cooperativeSession = false;
    bool cameraReady = false;
    bool reversedDepth = false;
    bool testMode = false;
    bool visibleOutline = false; // Legacy callers retain occluded-only behavior.
    bool showMask = false;
    float thickness = 0.0f; // Zero selects resolution-scaled default.
    bool fillSilhouette = false;
    float fillOpacity = 0.30f;
    bool encodedNearDepth = false;
    // Optional current-frame local-player coverage/depth, using the same encoding.
    ID3D11ShaderResourceView* localMask = nullptr;
};

class AllyOutline final {
public:
    HRESULT Initialize(ID3D11Device* device) noexcept;
    HRESULT Draw(ID3D11DeviceContext* immediate, const OutlineFrame& frame) noexcept;
    void Reset() noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> recording_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
};

// Receives draws whose native command packet belongs to a tracked ally or local player.
// The original skinned vertex pipeline is retained; color is composed once,
// after rendering, rather than being written into the game's G-buffer.
class LiveAllyOutline final {
public:
    static LiveAllyOutline& Instance() noexcept;
    bool Capture(ID3D11DeviceContext*, const ReplayDrawParams&, void* original, bool localPlayer = false) noexcept;
    HRESULT Present(IDXGISwapChain*, bool enabled, bool showMask = false, float thickness = 2.0f, bool visibleOutline = true, bool fillSilhouette = false, float fillOpacity = 0.30f, bool fallbackMarkers = true) noexcept;
    void BeforeDepthClear(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT flags, float depth) noexcept;
    void Finish(ID3D11DeviceContext*, ID3D11CommandList*) noexcept;
    bool Executed(ID3D11CommandList*) noexcept;
    void Reset() noexcept;
    [[nodiscard]] UINT CapturesThisFrame() const noexcept { return lastCaptures_; }
private:
    LiveAllyOutline() = default;
    HRESULT Initialize(ID3D11Device*, UINT width, UINT height) noexcept;
    HRESULT PrepareSceneCopy(ID3D11Texture2D*) noexcept;
    AllyOutline outline_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mask_, scene_, sceneCopy_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> localMask_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> localTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> localView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> persistentSceneCopy_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> maskTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskView_, sceneView_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> persistentSceneView_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> captureShader_, captureReversed_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> captureDepth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overwrite_;
    UINT width_ = 0, height_ = 0, captures_ = 0, lastCaptures_ = 0;
    UINT localCaptures_ = 0;
    std::uint64_t frame_ = 1, generation_ = 0;
    bool reversed_ = false, sceneFrozen_ = false, mixedScene_ = false;
    // The engine may transfer its immediate context between serialized threads.
    // Present thread identity is not a camera/context identity.
    std::recursive_mutex renderMutex_;
    struct DepthClear {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        float depth = 1;
    };
    std::array<DepthClear, 16> depthClears_{};
    UINT nextClear_ = 0;
    void EndFrame() noexcept;
    void RenderFallbackMarkers(ID3D11DeviceContext* context, ID3D11RenderTargetView* targetRTV) noexcept;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> markerVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> markerPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> markerConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> markerBlendState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> markerDepthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> markerRaster_;
};

} // namespace ds3sc::render
