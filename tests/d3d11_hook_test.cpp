#include "../src/render/d3d11_hook.h"
#include "../src/render/title_menu.h"
#include <cstdio>
#include <wrl/client.h>
#include <array>
#include <cstring>

static unsigned menuPresentCount = 0;

namespace ds3sc::render {
TitleMenu::TitleMenu() = default;
TitleMenu& TitleMenu::Instance() noexcept {
    static TitleMenu s;
    return s;
}
HRESULT TitleMenu::Present(IDXGISwapChain*) noexcept { ++menuPresentCount; return S_OK; }
void TitleMenu::Reset() noexcept {}
}

extern "C" volatile LONG ds3scAllyDeferredDraws, ds3scAllyMatchedDraws;
extern "C" volatile LONG ds3scD3D11HookRepairs;

int main() {
    using Microsoft::WRL::ComPtr;
    HWND window = CreateWindowExW(0, L"STATIC", L"DS3SC hook regression", WS_OVERLAPPEDWINDOW,
                                  0, 0, 160, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 1;
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 160;
    desc.BufferDesc.Height = 120;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferCount = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SampleDesc.Count = 1;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    auto hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device, nullptr, &context);
    if (FAILED(hr)) return 2;
    void** table = *reinterpret_cast<void***>(swap.Get());
    void* presentBefore = table[8];
    void* resizeBefore = table[13];
    std::array<unsigned char, 5> originalEntry{};
    std::memcpy(originalEntry.data(), presentBefore, originalEntry.size());
    auto& hooks = ds3sc::render::D3D11HookManager::Instance();
    if (!hooks.Install()) return 3;
    // Shared vtables must remain intact: overwriting them caused the recursive
    // Steam-overlay/companion Present chain in the real DS3 crash dump.
    if (table[8] != presentBefore || table[13] != resizeBefore) return 4;
    if (!hooks.Install()) return 5; // Installing twice must not hook our own hook.
    for (int frame = 0; frame < 120; ++frame) {
        if (FAILED(swap->Present(0, 0))) return 6;
    }
    if (ds3scD3D11Hooked != 1) return 7;
    // Reproduce the live failure: an overlay restores the original entry,
    // while MinHook continues reporting the detour as enabled.
    DWORD previousProtection = 0;
    if (!VirtualProtect(presentBefore, originalEntry.size(), PAGE_EXECUTE_READWRITE, &previousProtection)) return 15;
    std::memcpy(presentBefore, originalEntry.data(), originalEntry.size());
    VirtualProtect(presentBefore, originalEntry.size(), previousProtection, &previousProtection);
    FlushInstructionCache(GetCurrentProcess(), presentBefore, originalEntry.size());
    const auto menuBeforeRepair = menuPresentCount;
    if (FAILED(swap->Present(0, 0)) || menuPresentCount != menuBeforeRepair) return 16;
    hooks.MaintainPresentationHooks();
    if (ds3scD3D11HookRepairs != 1) return 17;
    if (FAILED(swap->Present(0, 0)) || menuPresentCount != menuBeforeRepair + 1) return 18;
    hooks.MaintainPresentationHooks();
    if (ds3scD3D11HookRepairs != 1) return 19;
    ComPtr<ID3D11DeviceContext> deferred;
    if (FAILED(device->CreateDeferredContext(0, &deferred))) return 12;
    const auto matchedBefore = ds3scAllyMatchedDraws;
    const auto deferredBefore = ds3scAllyDeferredDraws;
    ds3sc::render::SetDrawingAlly(true);
    deferred->Draw(0,0); deferred->DrawIndexed(0,0,0);
    deferred->DrawInstanced(0,1,0,0); deferred->DrawIndexedInstanced(0,1,0,0,0);
    ds3sc::render::SetDrawingAlly(false);
    if (ds3scAllyMatchedDraws - matchedBefore != 4 || ds3scAllyDeferredDraws - deferredBefore != 4) return 13;
    ComPtr<ID3D11CommandList> list;
    if (FAILED(deferred->FinishCommandList(FALSE, &list))) return 14;
    context->ExecuteCommandList(list.Get(), TRUE);
    list.Reset(); deferred.Reset();
    if (FAILED(swap->ResizeBuffers(1, 192, 128, DXGI_FORMAT_UNKNOWN, 0))) return 8;
    if (FAILED(swap->Present(0, 0))) return 9;
    hooks.Uninstall();
    if (hooks.IsInstalled() || table[8] != presentBefore || table[13] != resizeBefore) return 10;
    if (FAILED(swap->Present(0, 0))) return 11;
    swap.Reset();
    context.Reset();
    device.Reset();
    DestroyWindow(window);
    std::puts("PASS: hooked Presents/menu dispatch, late overlay restoration/recovery, deferred draws, stable vtables, resize and clean unhook.");
    return 0;
}
