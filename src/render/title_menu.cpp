#include "title_menu.h"
#include "actor_tracker.h"
#include "d3d11_hook.h"
#if (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS) || (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS)
#include "../extensions/counters/counters_extension.h"
#endif
#include "../../tools/vendor/minhook-1.3.4/include/MinHook.h"
#include <Xinput.h>

#include <d3dcompiler.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace ds3sc::render {

// Direct hook for DarkSoulsIII.exe native DLC action handler (Option 4 "Seamplus")
static bool (__fastcall *s_origDlcMenuAction)(void* self) = nullptr;

static bool __fastcall Hooked_DlcMenuAction(void* /*self*/) {
    TitleMenu::Instance().SetModalOpen(true);
    TitleMenu::Instance().LoadSettingsFromIni();
    return true; // Suppress native store overlay completely
}

// Hook for ISteamFriends::ActivateGameOverlayToStore and ActivateGameOverlay
using SteamFriendsFn = void* (*)();
static void (__fastcall *s_origActivateGameOverlayToStore)(void* self, std::uint32_t nAppId, int eFlag) = nullptr;
static void (__fastcall *s_origActivateGameOverlay)(void* self, const char* pchDialog) = nullptr;

static void __fastcall Hooked_ActivateGameOverlayToStore(void* /*self*/, std::uint32_t /*nAppId*/, int /*eFlag*/) {
    // Intercept native option 4 ("Seamplus") activation via gamepad or keyboard!
    TitleMenu::Instance().SetModalOpen(true);
    TitleMenu::Instance().LoadSettingsFromIni();
}

static void __fastcall Hooked_ActivateGameOverlay(void* self, const char* pchDialog) {
    if (pchDialog && (_stricmp(pchDialog, "Store") == 0 || _stricmp(pchDialog, "community") == 0)) {
        TitleMenu::Instance().SetModalOpen(true);
        TitleMenu::Instance().LoadSettingsFromIni();
        return;
    }
    if (s_origActivateGameOverlay) {
        s_origActivateGameOverlay(self, pchDialog);
    }
}

// Forward declare live exported mod flags from d3d11_hook.cpp
extern "C" {
#if defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE
    extern volatile LONG ds3scOutlineEnable;
    extern volatile LONG ds3scOutlineThicknessInt;
    extern volatile LONG ds3scOutlineFillSilhouette;
    extern volatile LONG ds3scOutlineFallbackMarkers;
#endif
#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
    extern volatile LONG ds3scDiamondMarkersEnable;
#endif
}

namespace {

struct Vertex {
    float x, y;       // Screen pixels
    float u, v;       // UV [0, 1]
    float r, g, b, a; // Color
    float mode;       // 0 = solid, 1 = font, 2 = radial glow
};

struct ConstantBufferData {
    float viewportWidth;
    float viewportHeight;
    float padding[2];
};

const char* kShadersHLSL = R"(
cbuffer ViewportBuffer : register(b0) {
    float viewportWidth;
    float viewportHeight;
    float2 padding;
};

Texture2D fontTexture : register(t0);
SamplerState fontSampler : register(s0);

struct VS_INPUT {
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float mode   : TEXCOORD1;
};

struct PS_INPUT {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float mode   : TEXCOORD1;
};

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    float ndcX = (input.pos.x / viewportWidth) * 2.0f - 1.0f;
    float ndcY = 1.0f - (input.pos.y / viewportHeight) * 2.0f;
    output.pos = float4(ndcX, ndcY, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    output.mode = input.mode;
    return output;
}

float4 PSMain(PS_INPUT input) : SV_TARGET {
    if (input.mode > 1.5f) {
        // Radial / Pill glow mode
        // uv is centered at (0, 0) in range [-1, 1]
        float dist = length(input.uv);
        float alpha = saturate(1.0f - dist * dist);
        alpha = alpha * alpha; // Smooth falloff
        return float4(input.color.rgb, input.color.a * alpha);
    }
    if (input.mode > 0.5f) {
        // Font glyph mode with crisp antialiased rendering
        float fontAlpha = fontTexture.Sample(fontSampler, input.uv).r;
        if (fontAlpha < 0.03f) discard;
        return float4(input.color.rgb, input.color.a * fontAlpha);
    }
    // Solid rectangle mode
    return input.color;
}
)";

std::wstring GetIniPath() noexcept {
    wchar_t buf[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetIniPath), &hMod);
    if (hMod) {
        GetModuleFileNameW(hMod, buf, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(buf, L'\\');
        if (lastSlash) {
            wcscpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - buf), L"ds3sc_settings.ini");
            return std::wstring(buf);
        }
    }
    if (GetFileAttributesW(L"SeamplusCoop\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
        return L"SeamplusCoop\\ds3sc_settings.ini";
    }
    return L"SeamlessCoop\\ds3sc_settings.ini";
}

[[maybe_unused]] bool IsTitleScreenActive() noexcept {
    auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"DarkSoulsIII.exe"));
    if (!gameBase) gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return true;

    // Check WorldChrMan (0x477FDB8u)
    std::uintptr_t worldChrMan = 0;
    if (!SafeRead(gameBase + 0x477FDB8u, worldChrMan) || !worldChrMan) {
        return true;
    }
    // Check local player pointer at WorldChrMan + 0x80
    std::uintptr_t localChr = 0;
    if (!SafeRead(worldChrMan + 0x80u, localChr) || !localChr) {
        return true;
    }

    return false;
}

} // namespace

TitleMenu& TitleMenu::Instance() noexcept {
    static TitleMenu instance;
    return instance;
}

TitleMenu::TitleMenu() {
    iniPath_ = GetIniPath();
    LoadSettingsFromIni();
}

void TitleMenu::EnsureSteamHook() noexcept {
    static bool hooked = false;
    if (hooked) return;

    MH_Initialize();

    // 1. Direct hook on DarkSoulsIII.exe native DLC action handler at RVA 0xEE1150
    // This triggers 100% reliably when option 4 ("Seamplus") is activated via gamepad, keyboard, or mouse!
    auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"DarkSoulsIII.exe"));
    if (!gameBase) gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (gameBase && !s_origDlcMenuAction) {
        auto* pTarget = reinterpret_cast<void*>(gameBase + 0xEE1150u);
        const auto* b = reinterpret_cast<const unsigned char*>(pTarget);
        if (b[0] == 0x40 && b[1] == 0x55 && b[2] == 0x48 && b[3] == 0x83 && b[4] == 0xEC) {
            MH_CreateHook(pTarget, reinterpret_cast<void*>(&Hooked_DlcMenuAction),
                          reinterpret_cast<void**>(&s_origDlcMenuAction));
            MH_EnableHook(pTarget);
        }
    }

    // 2. Steam overlay hooks (fallback for Steam overlay dialogs)
    HMODULE hSteam = GetModuleHandleW(L"steam_api64.dll");
    if (hSteam) {
        auto pfnSteamFriends = reinterpret_cast<SteamFriendsFn>(GetProcAddress(hSteam, "SteamFriends"));
        if (pfnSteamFriends) {
            void* friends = pfnSteamFriends();
            if (friends) {
                void** vtable = *reinterpret_cast<void***>(friends);
                if (vtable) {
                    if (vtable[24] && !s_origActivateGameOverlayToStore) {
                        MH_CreateHook(vtable[24], reinterpret_cast<void*>(&Hooked_ActivateGameOverlayToStore),
                                      reinterpret_cast<void**>(&s_origActivateGameOverlayToStore));
                        MH_EnableHook(vtable[24]);
                    }
                    if (vtable[21] && !s_origActivateGameOverlay) {
                        MH_CreateHook(vtable[21], reinterpret_cast<void*>(&Hooked_ActivateGameOverlay),
                                      reinterpret_cast<void**>(&s_origActivateGameOverlay));
                        MH_EnableHook(vtable[21]);
                    }
                }
            }
        }
    }

    hooked = true;
}

static WNDPROC s_origWndProc = nullptr;
static HWND s_hookedHwnd = nullptr;

static LRESULT CALLBACK Hooked_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (TitleMenu::Instance().IsModalOpen()) {
        switch (uMsg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            // Intercept all mouse clicks and scrolling so nothing behind the menu is clicked!
            return 0;

        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
            // Allow F7 through to toggle the menu open/closed
            if (wParam == VK_F7) {
                break;
            }
            return 0;
        }
    }

    if (uMsg == WM_DESTROY && s_origWndProc) {
        SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_origWndProc));
        s_hookedHwnd = nullptr;
    }

    return CallWindowProcW(s_origWndProc, hWnd, uMsg, wParam, lParam);
}

using XInputGetState_t = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
static XInputGetState_t s_origXInputGetState = nullptr;
static XInputGetState_t s_origXInputGetStateEx = nullptr;

static DWORD WINAPI Hooked_XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
    if (!s_origXInputGetState) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD ret = s_origXInputGetState(dwUserIndex, pState);
    if (ret == ERROR_SUCCESS && pState && TitleMenu::Instance().IsModalOpen()) {
        TitleMenu::Instance().ProcessGamepadInput(pState->Gamepad.wButtons);

        // Suppress inputs from reaching Dark Souls III behind the menu
        pState->Gamepad.wButtons = 0;
        pState->Gamepad.bLeftTrigger = 0;
        pState->Gamepad.bRightTrigger = 0;
        pState->Gamepad.sThumbLX = 0;
        pState->Gamepad.sThumbLY = 0;
        pState->Gamepad.sThumbRX = 0;
        pState->Gamepad.sThumbRY = 0;
    }
    return ret;
}

static DWORD WINAPI Hooked_XInputGetStateEx(DWORD dwUserIndex, XINPUT_STATE* pState) {
    if (!s_origXInputGetStateEx) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD ret = s_origXInputGetStateEx(dwUserIndex, pState);
    if (ret == ERROR_SUCCESS && pState && TitleMenu::Instance().IsModalOpen()) {
        TitleMenu::Instance().ProcessGamepadInput(pState->Gamepad.wButtons);

        pState->Gamepad.wButtons = 0;
        pState->Gamepad.bLeftTrigger = 0;
        pState->Gamepad.bRightTrigger = 0;
        pState->Gamepad.sThumbLX = 0;
        pState->Gamepad.sThumbLY = 0;
        pState->Gamepad.sThumbRX = 0;
        pState->Gamepad.sThumbRY = 0;
    }
    return ret;
}

void TitleMenu::EnsureInputHooks(HWND hWnd) noexcept {
    if (hWnd && hWnd != s_hookedHwnd) {
        s_hookedHwnd = hWnd;
        s_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Hooked_WndProc))
        );
    }

    static bool xinputHooked = false;
    if (!xinputHooked) {
        const char* const xinputDlls[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
        for (const char* dllName : xinputDlls) {
            HMODULE hXInput = GetModuleHandleA(dllName);
            if (!hXInput) hXInput = LoadLibraryA(dllName);
            if (hXInput) {
                auto pfnGetState = reinterpret_cast<XInputGetState_t>(GetProcAddress(hXInput, "XInputGetState"));
                if (pfnGetState && !s_origXInputGetState) {
                    MH_CreateHook(reinterpret_cast<void*>(pfnGetState),
                                  reinterpret_cast<void*>(&Hooked_XInputGetState),
                                  reinterpret_cast<void**>(&s_origXInputGetState));
                    MH_EnableHook(reinterpret_cast<void*>(pfnGetState));
                }
                auto pfnGetStateEx = reinterpret_cast<XInputGetState_t>(GetProcAddress(hXInput, reinterpret_cast<LPCSTR>(100)));
                if (pfnGetStateEx && !s_origXInputGetStateEx) {
                    MH_CreateHook(reinterpret_cast<void*>(pfnGetStateEx),
                                  reinterpret_cast<void*>(&Hooked_XInputGetStateEx),
                                  reinterpret_cast<void**>(&s_origXInputGetStateEx));
                    MH_EnableHook(reinterpret_cast<void*>(pfnGetStateEx));
                }
                xinputHooked = true;
                break;
            }
        }
    }
}

void TitleMenu::ProcessGamepadInput(unsigned short wButtons) noexcept {
    const auto now = GetTickCount64();
    if (now - lastGamepadTick_ < 180) return;

    if (wButtons & 0x0001 /* XINPUT_GAMEPAD_DPAD_UP */) {
        usingGamepadOrKeyboard_ = true;
        if (selectedItemIndex_ <= 0) selectedItemIndex_ = static_cast<int>(items_.size()) - 1;
        else --selectedItemIndex_;
        lastGamepadTick_ = now;
    } else if (wButtons & 0x0002 /* XINPUT_GAMEPAD_DPAD_DOWN */) {
        usingGamepadOrKeyboard_ = true;
        if (selectedItemIndex_ < 0 || selectedItemIndex_ >= static_cast<int>(items_.size()) - 1) selectedItemIndex_ = 0;
        else ++selectedItemIndex_;
        lastGamepadTick_ = now;
    } else if (wButtons & 0x0004 /* XINPUT_GAMEPAD_DPAD_LEFT */) {
        usingGamepadOrKeyboard_ = true;
        if (selectedItemIndex_ < 0) selectedItemIndex_ = 0;
        if (selectedItemIndex_ >= 0 && selectedItemIndex_ < static_cast<int>(items_.size())) {
            auto& it = items_[selectedItemIndex_];
            if (it.type == 0) it.valInt = (it.valInt == 0) ? 1 : 0;
            else if (it.type == 1) it.valInt = std::max(it.minInt, it.valInt - it.stepInt);
            UpdateItemDisplays();
            ApplyLiveSettings();
        }
        lastGamepadTick_ = now;
    } else if ((wButtons & 0x0008 /* XINPUT_GAMEPAD_DPAD_RIGHT */) || (wButtons & 0x1000 /* XINPUT_GAMEPAD_A */)) {
        usingGamepadOrKeyboard_ = true;
        if (selectedItemIndex_ < 0) selectedItemIndex_ = 0;
        if (selectedItemIndex_ >= 0 && selectedItemIndex_ < static_cast<int>(items_.size())) {
            auto& it = items_[selectedItemIndex_];
            if (it.type == 0) it.valInt = (it.valInt == 0) ? 1 : 0;
            else if (it.type == 1) it.valInt = std::min(it.maxInt, it.valInt + it.stepInt);
            UpdateItemDisplays();
            ApplyLiveSettings();
        }
        lastGamepadTick_ = now;
    } else if (wButtons & 0x2000 /* XINPUT_GAMEPAD_B */) {
        isModalOpen_ = false;
        lastGamepadTick_ = now;
    } else if (wButtons & 0x0010 /* XINPUT_GAMEPAD_START */) {
        SaveSettingsToIni();
        lastGamepadTick_ = now;
    }
}

void TitleMenu::LoadSettingsFromIni() noexcept {
    items_.clear();

    const auto readBool = [&](const wchar_t* sec, const wchar_t* key, int defVal) -> int {
        return static_cast<int>(GetPrivateProfileIntW(sec, key, defVal, iniPath_.c_str()));
    };
    const auto readString = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* defVal) -> std::string {
        wchar_t buf[128] = {};
        GetPrivateProfileStringW(sec, key, defVal, buf, ARRAYSIZE(buf), iniPath_.c_str());
        char outBuf[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, outBuf, sizeof(outBuf), nullptr, nullptr);
        return std::string(outBuf);
    };

    // 1. Ally Diamond Markers
#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
    items_.push_back({
        "Ally Diamond Markers", "", 0,
        readBool(L"ALLY_MARKERS", L"enabled", 1), 0, 1, 1,
        "ALLY_MARKERS", "enabled"
    });
#endif

#if defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE
    // 2. Ally Outline & Silhouette
    items_.push_back({
        "Ally Outline & Silhouette", "", 0,
        readBool(L"OUTLINE", L"show_ally_outline", 0), 0, 1, 1,
        "OUTLINE", "show_ally_outline"
    });

    // 3. Outline Thickness (10 = 1.0px, 20 = 2.0px, 30 = 3.0px, 40 = 4.0px)
    items_.push_back({
        "Outline Thickness", "", 1,
        20, 10, 40, 10,
        "OUTLINE", "outline_thickness"
    });

    // 4. Occluded Silhouette Fill
    items_.push_back({
        "Occluded Silhouette Fill", "", 0,
        readBool(L"OUTLINE", L"fill_silhouette", 0), 0, 1, 1,
        "OUTLINE", "fill_silhouette"
    });
#endif

#if (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS) || (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS)
    // 5. Combat Stats Overlay HUD
    items_.push_back({
        "Combat Stats Overlay", "", 0,
        readBool(L"COUNTERS", L"show_overlay", 0), 0, 1, 1,
        "COUNTERS", "show_overlay"
    });
#endif

    // 6. Skip Intro Logos
    items_.push_back({
        "Skip Intro Logos", "", 0,
        readBool(L"GAMEPLAY", L"skip_intros", 1), 0, 1, 1,
        "GAMEPLAY", "skip_intros"
    });

    // 7. Allow Invaders
    items_.push_back({
        "Allow Invaders", "", 0,
        readBool(L"GAMEPLAY", L"allow_invaders", 1), 0, 1, 1,
        "GAMEPLAY", "allow_invaders"
    });

    // 8. Death Debuffs (Rot Essence)
    items_.push_back({
        "Death Debuffs (Rot Essence)", "", 0,
        readBool(L"GAMEPLAY", L"death_debuffs", 1), 0, 1, 1,
        "GAMEPLAY", "death_debuffs"
    });

    // 9. Session Password
    std::string pwd = readString(L"PASSWORD", L"cooppassword", L"");
    if (pwd.empty()) pwd = "(No password)";
    items_.push_back({
        "Session Password", pwd, 2,
        0, 0, 0, 0,
        "PASSWORD", "cooppassword"
    });

    UpdateItemDisplays();
    selectedItemIndex_ = -1;
}

void TitleMenu::UpdateItemDisplays() noexcept {
    for (auto& it : items_) {
        if (it.type == 0) {
            it.valueDisplay = (it.valInt != 0) ? "< ENABLED >" : "< DISABLED >";
        } else if (it.type == 1) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "< %.1f px >", it.valInt / 10.0f);
            it.valueDisplay = buf;
        }
    }
}

void TitleMenu::SaveSettingsToIni() noexcept {
    for (const auto& it : items_) {
        if (it.type == 0 || it.type == 1) {
            wchar_t valStr[16] = {};
            swprintf_s(valStr, L"%d", it.valInt);
            wchar_t wSec[64] = {}, wKey[64] = {};
            MultiByteToWideChar(CP_UTF8, 0, it.section, -1, wSec, ARRAYSIZE(wSec));
            MultiByteToWideChar(CP_UTF8, 0, it.key, -1, wKey, ARRAYSIZE(wKey));
            WritePrivateProfileStringW(wSec, wKey, valStr, iniPath_.c_str());
        }
    }
    ApplyLiveSettings();
    saveStatusText_ = "SETTINGS SAVED & APPLIED";
    saveStatusTick_ = GetTickCount64();
}

void TitleMenu::ApplyLiveSettings() noexcept {
    for (const auto& it : items_) {
#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
        if (strcmp(it.key, "enabled") == 0 && strcmp(it.section, "ALLY_MARKERS") == 0) {
            ds3scDiamondMarkersEnable = it.valInt;
        }
#endif
#if defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE
        if (strcmp(it.key, "show_ally_outline") == 0) {
            ds3scOutlineEnable = it.valInt;
        } else if (strcmp(it.key, "outline_thickness") == 0) {
            ds3scOutlineThicknessInt = it.valInt;
        } else if (strcmp(it.key, "fill_silhouette") == 0) {
            ds3scOutlineFillSilhouette = it.valInt;
        }
#endif
#if (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS) || (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS)
        if (strcmp(it.key, "show_overlay") == 0 && strcmp(it.section, "COUNTERS") == 0) {
            auto* ext = extensions::GetCountersInstance();
            if (ext) ext->SetOverlayVisible(it.valInt != 0);
        }
#endif
    }
}

float TitleMenu::GetTextWidth(const char* str, float scale) const noexcept {
    if (!str) return 0.0f;
    float w = 0.0f;
    for (const char* p = str; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 32 || c > 126) c = '?';
        const int idx = c - 32;
        const int cw = (charWidths_[idx] > 0) ? charWidths_[idx] : 8;
        w += static_cast<float>(cw) * scale;
    }
    return w;
}

void TitleMenu::Reset() noexcept {
    rtv_.Reset();
    vertexBuffer_.Reset();
    constantBuffer_.Reset();
    fontTexture_.Reset();
    fontSRV_.Reset();
    fontSampler_.Reset();
    blendState_.Reset();
    depthState_.Reset();
    rasterState_.Reset();
    inputLayout_.Reset();
    pixelShader_.Reset();
    vertexShader_.Reset();
    device_.Reset();
    width_ = 0;
    height_ = 0;
}

HRESULT TitleMenu::EnsureResources(ID3D11Device* device, UINT width, UINT height) noexcept {
    if (device_.Get() == device && width_ == width && height_ == height && vertexBuffer_) {
        return S_OK;
    }
    device_ = device;
    width_ = width;
    height_ = height;

    // Compile Shaders
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    HRESULT hr = D3DCompile(kShadersHLSL, strlen(kShadersHLSL), "TitleMenuVS", nullptr, nullptr,
                            "VSMain", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) return hr;

    hr = D3DCompile(kShadersHLSL, strlen(kShadersHLSL), "TitleMenuPS", nullptr, nullptr,
                    "PSMain", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) return hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader_);
    if (FAILED(hr)) return hr;

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader_);
    if (FAILED(hr)) return hr;

    // Input Layout
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION",  0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",     0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  1, DXGI_FORMAT_R32_FLOAT,          0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    hr = device->CreateInputLayout(layoutDesc, 4, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_);
    if (FAILED(hr)) return hr;

    // Vertex Buffer
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(Vertex) * 6000;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&vbDesc, nullptr, &vertexBuffer_);
    if (FAILED(hr)) return hr;

    // Constant Buffer
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(ConstantBufferData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, nullptr, &constantBuffer_);
    if (FAILED(hr)) return hr;

    // Generate clean antialiased font atlas using Win32 GDI
    constexpr int kAtlasW = 512;
    constexpr int kAtlasH = 256;
    constexpr int kCellW = 32;
    constexpr int kCellH = 36;
    constexpr int kPadX = 3;
    constexpr int kPadY = 3;
    constexpr int kFontHeight = 20;

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = kAtlasW;
    texDesc.Height = kAtlasH;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<unsigned char> texData(kAtlasW * kAtlasH, 0);

    HDC hdc = CreateCompatibleDC(nullptr);
    if (hdc) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kAtlasW;
        bmi.bmiHeader.biHeight = -kAtlasH; // Top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        if (hBmp && pBits) {
            HGDIOBJ hOldBmp = SelectObject(hdc, hBmp);
            std::memset(pBits, 0, kAtlasW * kAtlasH * 4);

            HFONT hFont = CreateFontW(-kFontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (!hFont) {
                hFont = CreateFontW(-kFontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
            }
            HGDIOBJ hOldFont = SelectObject(hdc, hFont);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));

            int widths[95] = {};
            GetCharWidth32W(hdc, 32, 126, widths);
            for (int i = 0; i < 95; ++i) {
                charWidths_[i] = widths[i];
            }

            for (int ch = 33; ch <= 126; ++ch) {
                const int idx = ch - 32;
                const int col = idx % 16;
                const int row = idx / 16;
                const int cx = col * kCellW + kPadX;
                const int cy = row * kCellH + kPadY;
                RECT rc{ cx, cy, cx + kCellW, cy + kCellH };
                wchar_t wch = static_cast<wchar_t>(ch);
                DrawTextW(hdc, &wch, 1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
            }

            const auto* pPixels = static_cast<const std::uint32_t*>(pBits);
            for (int i = 0; i < kAtlasW * kAtlasH; ++i) {
                // For white text on black background, G channel is luminance
                texData[i] = static_cast<unsigned char>((pPixels[i] >> 8) & 0xFF);
            }

            SelectObject(hdc, hOldFont);
            if (hFont) DeleteObject(hFont);
            SelectObject(hdc, hOldBmp);
            DeleteObject(hBmp);
        }
        DeleteDC(hdc);
    }

    D3D11_SUBRESOURCE_DATA subData{};
    subData.pSysMem = texData.data();
    subData.SysMemPitch = kAtlasW;
    hr = device->CreateTexture2D(&texDesc, &subData, &fontTexture_);
    if (FAILED(hr)) return hr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(fontTexture_.Get(), &srvDesc, &fontSRV_);
    if (FAILED(hr)) return hr;

    // Linear Font Sampler
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device->CreateSamplerState(&sampDesc, &fontSampler_);
    if (FAILED(hr)) return hr;

    // Blend State (Premultiplied / Alpha Blend)
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

    // Depth Stencil (Disabled)
    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = FALSE;
    dsDesc.StencilEnable = FALSE;
    hr = device->CreateDepthStencilState(&dsDesc, &depthState_);
    if (FAILED(hr)) return hr;

    // Rasterizer State (Cull None)
    D3D11_RASTERIZER_DESC rDesc{};
    rDesc.FillMode = D3D11_FILL_SOLID;
    rDesc.CullMode = D3D11_CULL_NONE;
    rDesc.DepthClipEnable = FALSE;
    hr = device->CreateRasterizerState(&rDesc, &rasterState_);
    return hr;
}

HRESULT TitleMenu::Present(IDXGISwapChain* swapChain) noexcept {
    if (!swapChain) return S_OK;

    EnsureSteamHook();

    // Allow opening via F7 hotkey even if in game
    const auto now = GetTickCount64();
    if ((GetAsyncKeyState(VK_F7) & 0x8000) && (now - lastInputTick_ > 300)) {
        isModalOpen_ = !isModalOpen_;
        lastInputTick_ = now;
        if (isModalOpen_) LoadSettingsFromIni();
    }

    if (!isModalOpen_) {
        return S_OK;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device) return S_OK;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) return S_OK;

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);

    if (FAILED(EnsureResources(device.Get(), bbDesc.Width, bbDesc.Height))) return S_OK;

    if (!rtv_) {
        if (FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_))) return S_OK;
    }

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx) return S_OK;

    // Query cursor position
    DXGI_SWAP_CHAIN_DESC scDesc{};
    swapChain->GetDesc(&scDesc);
    HWND hWnd = scDesc.OutputWindow;
    if (hWnd) {
        EnsureInputHooks(hWnd);
    }
    POINT mousePt{ -1, -1 };
    if (hWnd && GetCursorPos(&mousePt)) {
        ScreenToClient(hWnd, &mousePt);
    }
    const float mx = static_cast<float>(mousePt.x);
    const float my = static_cast<float>(mousePt.y);

    static bool s_prevLButton = false;
    const bool curLButton = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool mouseClicked = curLButton && !s_prevLButton && (now - modalOpenTick_ > 150);
    s_prevLButton = curLButton;

    static POINT s_lastMousePt{ -9999, -9999 };
    const bool mouseMoved = (mousePt.x != s_lastMousePt.x || mousePt.y != s_lastMousePt.y);
    if (mouseMoved) {
        s_lastMousePt = mousePt;
    }
    if (mouseMoved || mouseClicked) {
        usingGamepadOrKeyboard_ = false;
    }

    // Save full DirectX 11 pipeline state
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

    // Build geometry list
    std::vector<Vertex> vertices;
    vertices.reserve(4000);

    auto addRect = [&](float rx, float ry, float rw, float rh, float cr, float cg, float cb, float ca) {
        Vertex v0{ rx,      ry,      0.0f, 0.0f, cr, cg, cb, ca, 0.0f };
        Vertex v1{ rx + rw, ry,      0.0f, 0.0f, cr, cg, cb, ca, 0.0f };
        Vertex v2{ rx,      ry + rh, 0.0f, 0.0f, cr, cg, cb, ca, 0.0f };
        Vertex v3{ rx + rw, ry + rh, 0.0f, 0.0f, cr, cg, cb, ca, 0.0f };
        vertices.push_back(v0); vertices.push_back(v1); vertices.push_back(v2);
        vertices.push_back(v1); vertices.push_back(v3); vertices.push_back(v2);
    };

    auto addGlow = [&](float cx, float cy, float rx, float ry, float cr, float cg, float cb, float ca) {
        Vertex v0{ cx - rx, cy - ry, -1.0f, -1.0f, cr, cg, cb, ca, 2.0f };
        Vertex v1{ cx + rx, cy - ry,  1.0f, -1.0f, cr, cg, cb, ca, 2.0f };
        Vertex v2{ cx - rx, cy + ry, -1.0f,  1.0f, cr, cg, cb, ca, 2.0f };
        Vertex v3{ cx + rx, cy + ry,  1.0f,  1.0f, cr, cg, cb, ca, 2.0f };
        vertices.push_back(v0); vertices.push_back(v1); vertices.push_back(v2);
        vertices.push_back(v1); vertices.push_back(v3); vertices.push_back(v2);
    };

    auto addText = [&](float tx, float ty, const char* str, float scale,
                       float cr, float cg, float cb, float ca) {
        if (!str) return;
        float curX = tx;
        constexpr float kAtlasW = 512.0f;
        constexpr float kAtlasH = 256.0f;
        constexpr float kCellW = 32.0f;
        constexpr float kCellH = 36.0f;
        constexpr float kPadX = 3.0f;
        constexpr float kPadY = 3.0f;
        constexpr float kQuadH = 28.0f;

        for (const char* p = str; *p; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c < 32 || c > 126) c = '?';
            const int idx = c - 32;
            const float cw = (charWidths_[idx] > 0) ? static_cast<float>(charWidths_[idx]) : 8.0f;
            const float advW = cw * scale;

            if (c == ' ') {
                curX += advW;
                continue;
            }

            const int col = idx % 16;
            const int row = idx / 16;
            const float u0 = (static_cast<float>(col) * kCellW + kPadX) / kAtlasW;
            const float v0 = (static_cast<float>(row) * kCellH + kPadY) / kAtlasH;
            const float u1 = (static_cast<float>(col) * kCellW + kPadX + cw) / kAtlasW;
            const float v1 = (static_cast<float>(row) * kCellH + kPadY + kQuadH) / kAtlasH;

            const float charW = std::floor(advW + 0.5f);
            const float charH = std::floor(kQuadH * scale + 0.5f);
            const float drawX = std::floor(curX);
            const float drawY = std::floor(ty);

            Vertex v0_{ drawX,         drawY,         u0, v0, cr, cg, cb, ca, 1.0f };
            Vertex v1_{ drawX + charW, drawY,         u1, v0, cr, cg, cb, ca, 1.0f };
            Vertex v2_{ drawX,         drawY + charH, u0, v1, cr, cg, cb, ca, 1.0f };
            Vertex v3_{ drawX + charW, drawY + charH, u1, v1, cr, cg, cb, ca, 1.0f };

            vertices.push_back(v0_); vertices.push_back(v1_); vertices.push_back(v2_);
            vertices.push_back(v1_); vertices.push_back(v3_); vertices.push_back(v2_);

            curX += advW;
        }
    };

    const float sw = static_cast<float>(bbDesc.Width);
    const float sh = static_cast<float>(bbDesc.Height);

    // =========================================================================
    // Interactive Seamplus Settings Modal
    // (Native menu item 4 is now displayed as "Seamplus" by Dark Souls III itself)
    // =========================================================================
    if (isModalOpen_) {
        // Keyboard inputs for navigation
        if (now - lastInputTick_ > 150) {
            if ((GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState('W') & 0x8000)) {
                usingGamepadOrKeyboard_ = true;
                if (selectedItemIndex_ <= 0) selectedItemIndex_ = static_cast<int>(items_.size()) - 1;
                else --selectedItemIndex_;
                lastInputTick_ = now;
            } else if ((GetAsyncKeyState(VK_DOWN) & 0x8000) || (GetAsyncKeyState('S') & 0x8000)) {
                usingGamepadOrKeyboard_ = true;
                if (selectedItemIndex_ < 0 || selectedItemIndex_ >= static_cast<int>(items_.size()) - 1) selectedItemIndex_ = 0;
                else ++selectedItemIndex_;
                lastInputTick_ = now;
            } else if ((GetAsyncKeyState(VK_LEFT) & 0x8000) || (GetAsyncKeyState('A') & 0x8000)) {
                usingGamepadOrKeyboard_ = true;
                if (selectedItemIndex_ < 0) selectedItemIndex_ = 0;
                if (selectedItemIndex_ >= 0 && selectedItemIndex_ < static_cast<int>(items_.size())) {
                    auto& it = items_[selectedItemIndex_];
                    if (it.type == 0) {
                        it.valInt = (it.valInt == 0) ? 1 : 0;
                    } else if (it.type == 1) {
                        it.valInt = std::max(it.minInt, it.valInt - it.stepInt);
                    }
                    UpdateItemDisplays();
                    ApplyLiveSettings();
                }
                lastInputTick_ = now;
            } else if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) || (GetAsyncKeyState('D') & 0x8000) ||
                       (GetAsyncKeyState(VK_RETURN) & 0x8000) || (GetAsyncKeyState(VK_SPACE) & 0x8000)) {
                usingGamepadOrKeyboard_ = true;
                if (selectedItemIndex_ < 0) selectedItemIndex_ = 0;
                if (selectedItemIndex_ >= 0 && selectedItemIndex_ < static_cast<int>(items_.size())) {
                    auto& it = items_[selectedItemIndex_];
                    if (it.type == 0) {
                        it.valInt = (it.valInt == 0) ? 1 : 0;
                    } else if (it.type == 1) {
                        it.valInt = std::min(it.maxInt, it.valInt + it.stepInt);
                    }
                    UpdateItemDisplays();
                    ApplyLiveSettings();
                }
                lastInputTick_ = now;
            } else if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                isModalOpen_ = false;
                lastInputTick_ = now;
            }
        }

        // Fullscreen dark translucent backdrop
        addRect(0.0f, 0.0f, sw, sh, 0.0f, 0.0f, 0.0f, 0.72f);

        // Modal Box Dimensions (dynamically adapted to compiled items)
        const float itemH = 36.0f;
        const float contentH = static_cast<float>(items_.size()) * itemH;
        const float boxW = (sw >= 1440.0f) ? 760.0f : 680.0f;
        const float boxH = std::clamp(160.0f + contentH, 340.0f, 620.0f);
        const float boxX = (sw - boxW) * 0.5f;
        const float boxY = (sh - boxH) * 0.5f;
        constexpr float borderW = 2.0f;

        // Dark charcoal FromSoft background (#0c0c0e @ 94%)
        addRect(boxX, boxY, boxW, boxH, 0.05f, 0.05f, 0.06f, 0.94f);

        // Outer ashen gold border (#9e8c63)
        addRect(boxX, boxY, boxW, borderW, 0.62f, 0.55f, 0.39f, 0.95f);
        addRect(boxX, boxY + boxH - borderW, boxW, borderW, 0.62f, 0.55f, 0.39f, 0.95f);
        addRect(boxX, boxY, borderW, boxH, 0.62f, 0.55f, 0.39f, 0.95f);
        addRect(boxX + boxW - borderW, boxY, borderW, boxH, 0.62f, 0.55f, 0.39f, 0.95f);

        // Header dividing line
        addRect(boxX + 16.0f, boxY + 46.0f, boxW - 32.0f, 1.0f, 0.55f, 0.48f, 0.35f, 0.75f);

        // Modal Header Title
        addText(boxX + 24.0f, boxY + 14.0f, "SEAMPLUS - SETTINGS", 0.90f, 0.92f, 0.82f, 0.58f, 1.0f);

        // Render Options List
        float itemY = boxY + 58.0f;

        for (size_t i = 0; i < items_.size(); ++i) {
            auto& it = items_[i];
            const bool isSelected = usingGamepadOrKeyboard_ && (static_cast<int>(i) == selectedItemIndex_);
            const bool isHovered = (mx >= boxX + 16.0f && mx <= boxX + boxW - 16.0f &&
                                    my >= itemY && my <= itemY + itemH);

            if (isHovered && mouseClicked) {
                usingGamepadOrKeyboard_ = false;
                selectedItemIndex_ = static_cast<int>(i);
                if (it.type == 0) {
                    it.valInt = (it.valInt == 0) ? 1 : 0;
                } else if (it.type == 1) {
                    // Click left half or right half
                    if (mx > boxX + boxW - 120.0f) {
                        it.valInt = std::min(it.maxInt, it.valInt + it.stepInt);
                    } else {
                        it.valInt = std::max(it.minInt, it.valInt - it.stepInt);
                    }
                }
                UpdateItemDisplays();
                ApplyLiveSettings();
            }

            const bool isHighlighted = isHovered || isSelected;

            // Selection / Hover row highlight (clean FromSoftware subtle bar without round radio dots)
            if (isHighlighted) {
                addRect(boxX + 12.0f, itemY + 2.0f, boxW - 24.0f, itemH - 4.0f, 0.28f, 0.20f, 0.08f, 0.50f);
                addRect(boxX + 12.0f, itemY + 2.0f, 3.0f, itemH - 4.0f, 0.92f, 0.78f, 0.42f, 0.95f);
            }

            // Item Name
            const float textColR = isHighlighted ? 1.0f : 0.82f;
            const float textColG = isHighlighted ? 0.92f : 0.80f;
            const float textColB = isHighlighted ? 0.70f : 0.74f;
            addText(boxX + 28.0f, itemY + 7.0f, it.name.c_str(), 0.75f, textColR, textColG, textColB, 1.0f);

            // Item Value (Right aligned)
            const float valW = GetTextWidth(it.valueDisplay.c_str(), 0.75f);
            const float valX = boxX + boxW - valW - 36.0f;

            float valColR = 0.85f, valColG = 0.85f, valColB = 0.85f;
            if (it.type == 0) {
                if (it.valInt != 0) {
                    valColR = 0.45f; valColG = 0.90f; valColB = 0.45f; // Active green
                } else {
                    valColR = 0.88f; valColG = 0.35f; valColB = 0.35f; // Inactive soft red
                }
            } else if (it.type == 1) {
                valColR = 0.95f; valColG = 0.82f; valColB = 0.45f; // Gold
            }
            addText(valX, itemY + 7.0f, it.valueDisplay.c_str(), 0.75f, valColR, valColG, valColB, 1.0f);

            itemY += itemH;
        }

        // Bottom footer line
        addRect(boxX + 16.0f, boxY + boxH - 66.0f, boxW - 32.0f, 1.0f, 0.42f, 0.38f, 0.32f, 0.65f);

        // Status Message (if saved)
        if (!saveStatusText_.empty() && (now - saveStatusTick_ < 3500)) {
            addText(boxX + 28.0f, boxY + boxH - 92.0f, saveStatusText_.c_str(), 0.72f, 0.40f, 0.95f, 0.50f, 1.0f);
        }

        // Button 1: [ SAVE & APPLY ]
        const float btn1X = boxX + 28.0f;
        const float btn1Y = boxY + boxH - 50.0f;
        const float btn1W = 180.0f;
        const float btn1H = 34.0f;
        const bool btn1Hovered = (mx >= btn1X && mx <= btn1X + btn1W && my >= btn1Y && my <= btn1Y + btn1H);

        if (btn1Hovered && mouseClicked) {
            SaveSettingsToIni();
        }

        addRect(btn1X, btn1Y, btn1W, btn1H, 0.12f, 0.11f, 0.08f, 0.90f);
        addRect(btn1X, btn1Y, btn1W, 1.0f, btn1Hovered ? 0.95f : 0.55f, btn1Hovered ? 0.78f : 0.46f, 0.28f, 0.95f);
        addRect(btn1X, btn1Y + btn1H - 1.0f, btn1W, 1.0f, btn1Hovered ? 0.95f : 0.55f, btn1Hovered ? 0.78f : 0.46f, 0.28f, 0.95f);
        addRect(btn1X, btn1Y, 1.0f, btn1H, btn1Hovered ? 0.95f : 0.55f, btn1Hovered ? 0.78f : 0.46f, 0.28f, 0.95f);
        addRect(btn1X + btn1W - 1.0f, btn1Y, 1.0f, btn1H, btn1Hovered ? 0.95f : 0.55f, btn1Hovered ? 0.78f : 0.46f, 0.28f, 0.95f);
        const float b1w = GetTextWidth("SAVE & APPLY", 0.75f);
        addText(btn1X + (btn1W - b1w) * 0.5f, btn1Y + 6.0f, "SAVE & APPLY", 0.75f, btn1Hovered ? 1.0f : 0.85f, btn1Hovered ? 0.90f : 0.78f, btn1Hovered ? 0.60f : 0.58f, 1.0f);

        // Button 2: [ CLOSE (ESC) ]
        const float btn2X = boxX + boxW - 188.0f;
        const float btn2Y = boxY + boxH - 50.0f;
        const float btn2W = 160.0f;
        const float btn2H = 34.0f;
        const bool btn2Hovered = (mx >= btn2X && mx <= btn2X + btn2W && my >= btn2Y && my <= btn2Y + btn2H);

        if (btn2Hovered && mouseClicked) {
            isModalOpen_ = false;
        }

        addRect(btn2X, btn2Y, btn2W, btn2H, 0.12f, 0.11f, 0.08f, 0.90f);
        addRect(btn2X, btn2Y, btn2W, 1.0f, btn2Hovered ? 0.88f : 0.50f, btn2Hovered ? 0.85f : 0.46f, 0.40f, 0.95f);
        addRect(btn2X, btn2Y + btn2H - 1.0f, btn2W, 1.0f, btn2Hovered ? 0.88f : 0.50f, btn2Hovered ? 0.85f : 0.46f, 0.40f, 0.95f);
        addRect(btn2X, btn2Y, 1.0f, btn2H, btn2Hovered ? 0.88f : 0.50f, btn2Hovered ? 0.85f : 0.46f, 0.40f, 0.95f);
        addRect(btn2X + btn2W - 1.0f, btn2Y, 1.0f, btn2H, btn2Hovered ? 0.88f : 0.50f, btn2Hovered ? 0.85f : 0.46f, 0.40f, 0.95f);
        const float b2w = GetTextWidth("CLOSE (ESC)", 0.75f);
        addText(btn2X + (btn2W - b2w) * 0.5f, btn2Y + 6.0f, "CLOSE (ESC)", 0.75f, btn2Hovered ? 1.0f : 0.85f, btn2Hovered ? 0.88f : 0.78f, btn2Hovered ? 0.65f : 0.60f, 1.0f);
    }

    // Upload vertices and dispatch draw
    if (!vertices.empty()) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const UINT copyCount = (vertices.size() < 6000) ? static_cast<UINT>(vertices.size()) : 6000;
            std::memcpy(mapped.pData, vertices.data(), sizeof(Vertex) * copyCount);
            ctx->Unmap(vertexBuffer_.Get(), 0);

            // Upload viewport constants
            if (SUCCEEDED(ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                auto* cb = static_cast<ConstantBufferData*>(mapped.pData);
                cb->viewportWidth = sw;
                cb->viewportHeight = sh;
                ctx->Unmap(constantBuffer_.Get(), 0);
            }

            // Viewport
            D3D11_VIEWPORT vp{};
            vp.Width = sw;
            vp.Height = sh;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);

            // Render Target
            ID3D11RenderTargetView* targets[1] = { rtv_.Get() };
            ctx->OMSetRenderTargets(1, targets, nullptr);

            // States
            FLOAT blendFactors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            ctx->OMSetBlendState(blendState_.Get(), blendFactors, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(depthState_.Get(), 0);
            ctx->RSSetState(rasterState_.Get());

            // Shaders
            ctx->VSSetShader(vertexShader_.Get(), nullptr, 0);
            ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
            ctx->VSSetConstantBuffers(0, 1, cbs);

            ctx->PSSetShader(pixelShader_.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvs[1] = { fontSRV_.Get() };
            ctx->PSSetShaderResources(0, 1, srvs);
            ID3D11SamplerState* samps[1] = { fontSampler_.Get() };
            ctx->PSSetSamplers(0, 1, samps);

            // Draw call
            ctx->IASetInputLayout(inputLayout_.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            UINT stride = sizeof(Vertex);
            UINT offset = 0;
            ID3D11Buffer* vbs[1] = { vertexBuffer_.Get() };
            ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);

            ctx->Draw(copyCount, 0);
        }
    }

    // Restore full DirectX 11 pipeline state
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
