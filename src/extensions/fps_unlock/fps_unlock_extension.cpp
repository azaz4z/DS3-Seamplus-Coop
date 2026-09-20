#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "fps_unlock_extension.h"
#include "../../render/d3d11_hook.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>

// Forward declaration of VSync toggle export from d3d11_hook.cpp
extern "C" {
    extern volatile LONG ds3scDisableVsync;
}

namespace ds3sc::extensions {

namespace {

FpsUnlockExtension* g_fpsUnlockInstance = nullptr;

inline bool IsValidUserPointer(const void* ptr, std::size_t size = sizeof(void*)) noexcept {
    const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
    return (addr >= 0x10000u && (addr + size) >= addr && (addr + size) <= 0x7FFFFFFEFFFFu);
}

template<typename T>
inline bool SafeRead(std::uintptr_t addr, T& out) noexcept {
    if (!IsValidUserPointer(reinterpret_cast<const void*>(addr), sizeof(T))) return false;
    __try {
        std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template<typename T>
inline bool SafeWrite(std::uintptr_t addr, const T& val) noexcept {
    if (!IsValidUserPointer(reinterpret_cast<const void*>(addr), sizeof(T))) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(addr), &val, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t ScanMemoryPattern(std::uintptr_t start, std::size_t size,
                                 const unsigned char* pattern, std::size_t patternLen) noexcept {
    if (!start || size < patternLen) return 0;
    const unsigned char* pStart = reinterpret_cast<const unsigned char*>(start);
    for (std::size_t i = 0; i <= size - patternLen; ++i) {
        if (pStart[i] == pattern[0] && std::memcmp(pStart + i, pattern, patternLen) == 0) {
            return start + i;
        }
    }
    return 0;
}

} // namespace

FpsUnlockExtension::FpsUnlockExtension() noexcept {
    g_fpsUnlockInstance = this;
}

void FpsUnlockExtension::LocateSprjFlipper() noexcept {
    if (sprjFlipperPtrAddress_ != 0) return;

    auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"DarkSoulsIII.exe"));
    if (!gameBase) gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return;

    // Pattern in DarkSoulsIII.exe:
    // C6 80 80 00 00 00 01 48 8B 05 [disp32]
    // Instruction: mov byte ptr [rax + 0x80], 1
    // Followed by: mov rax, qword ptr [rip + disp32] (offset 7)
    static const unsigned char kPattern[] = {
        0xC6, 0x80, 0x80, 0x00, 0x00, 0x00, 0x01, 0x48, 0x8B, 0x05
    };

    // Parse PE headers to find .text section
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameBase);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(gameBase + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
            const auto* section = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
                if (std::strncmp(reinterpret_cast<const char*>(section->Name), ".text", 5) == 0) {
                    const std::uintptr_t textStart = gameBase + section->VirtualAddress;
                    const std::size_t textSize = section->Misc.VirtualSize;
                    const std::uintptr_t match = ScanMemoryPattern(textStart, textSize, kPattern, sizeof(kPattern));
                    if (match != 0) {
                        // Offset 7 is instruction 48 8B 05 [disp32] (length 7)
                        const std::uintptr_t instAddr = match + 7;
                        std::int32_t disp = 0;
                        if (SafeRead(instAddr + 3, disp)) {
                            sprjFlipperPtrAddress_ = instAddr + 7 + disp;
                            OutputDebugStringA("[fps_unlock] SprjFlipper pointer located via AOB scan.\n");
                            return;
                        }
                    }
                    break;
                }
            }
        }
    }

    // Direct RVA fallback for known DarkSoulsIII.exe releases
    // v1.15.2: 0x489DD10, v1.15.1: 0x489DD20
    const std::uintptr_t fallbackRva = 0x489DD10;
    sprjFlipperPtrAddress_ = gameBase + fallbackRva;
    OutputDebugStringA("[fps_unlock] SprjFlipper pointer set using v1.15.2 RVA fallback.\n");
}

void FpsUnlockExtension::LoadSettings() noexcept {
    iniPath_[0] = '\0';
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CreateFpsUnlockExtension), &hMod) && hMod) {
        GetModuleFileNameA(hMod, iniPath_, sizeof(iniPath_));
        char* lastSlash = std::strrchr(iniPath_, '\\');
        if (lastSlash) {
            strcpy_s(lastSlash + 1, sizeof(iniPath_) - (lastSlash + 1 - iniPath_), "ds3sc_settings.ini");
        }
    }
    if (iniPath_[0] == '\0') {
        if (GetFileAttributesA("TheAshenLink\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath_, "TheAshenLink\\ds3sc_settings.ini");
        } else if (GetFileAttributesA("SeamplusCoop\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath_, "SeamplusCoop\\ds3sc_settings.ini");
        } else {
            strcpy_s(iniPath_, "SeamlessCoop\\ds3sc_settings.ini");
        }
    }

    const int unlock = GetPrivateProfileIntA("FPS", "unlock_fps", 0, iniPath_);
    enabled_.store(unlock != 0, std::memory_order_relaxed);

    char fpsBuf[32] = {};
    GetPrivateProfileStringA("FPS", "target_fps", "144.0", fpsBuf, sizeof(fpsBuf), iniPath_);
    float fps = static_cast<float>(std::atof(fpsBuf));
    if (fps < 30.0f) fps = 144.0f;
    targetFps_.store(fps, std::memory_order_relaxed);

    const int vsync = GetPrivateProfileIntA("FPS", "vsync", -1, iniPath_);
    if (vsync != -1) {
        disableVsync_.store(vsync == 0, std::memory_order_relaxed);
    } else {
        const int noVsync = GetPrivateProfileIntA("FPS", "disable_vsync", 0, iniPath_);
        disableVsync_.store(noVsync != 0, std::memory_order_relaxed);
    }
}

bool FpsUnlockExtension::Initialize() noexcept {
    LoadSettings();
    LocateSprjFlipper();

    // Ensure D3D11 hook is active for VSync control and title menu
    render::D3D11HookManager::Instance().Install();

    ApplySettings();
    initialized_.store(true, std::memory_order_relaxed);
    OutputDebugStringA("[fps_unlock] FPS Unlocker extension initialized successfully.\n");
    return true;
}

void FpsUnlockExtension::Shutdown() noexcept {
    // Revert SprjFlipper to default 60 FPS cap
    if (sprjFlipperPtrAddress_ != 0) {
        std::uintptr_t flipper = 0;
        if (SafeRead(sprjFlipperPtrAddress_, flipper) && flipper != 0) {
            const std::uint8_t useDebug = 0;
            const float defaultFps = 60.0f;
            SafeWrite(flipper + 0x358, useDebug);
            SafeWrite(flipper + 0x354, defaultFps);
        }
    }

    // Revert VSync override
    InterlockedExchange(&ds3scDisableVsync, 0);

    initialized_.store(false, std::memory_order_relaxed);
}

void FpsUnlockExtension::ApplySettings() noexcept {
    if (sprjFlipperPtrAddress_ == 0) {
        LocateSprjFlipper();
    }

    if (sprjFlipperPtrAddress_ != 0) {
        std::uintptr_t flipper = 0;
        if (SafeRead(sprjFlipperPtrAddress_, flipper) && flipper != 0) {
            if (enabled_.load(std::memory_order_relaxed)) {
                float target = targetFps_.load(std::memory_order_relaxed);
                if (target <= 0.0f) target = 360.0f;
                const std::uint8_t useDebug = 1;
                SafeWrite(flipper + 0x354, target);
                SafeWrite(flipper + 0x358, useDebug);
            } else {
                const std::uint8_t useDebug = 0;
                const float defaultFps = 60.0f;
                SafeWrite(flipper + 0x358, useDebug);
                SafeWrite(flipper + 0x354, defaultFps);
            }
        }
    }

    // Update VSync override in D3D11 hook
    InterlockedExchange(&ds3scDisableVsync, disableVsync_.load(std::memory_order_relaxed) ? 1 : 0);
}

void FpsUnlockExtension::OnTick() noexcept {
    const auto now = GetTickCount64();
    if (now - lastTickMs_ < 200) return;
    lastTickMs_ = now;

    if (sprjFlipperPtrAddress_ == 0) {
        LocateSprjFlipper();
        if (sprjFlipperPtrAddress_ == 0) return;
    }

    std::uintptr_t flipper = 0;
    if (!SafeRead(sprjFlipperPtrAddress_, flipper) || flipper == 0) return;

    std::uint8_t curDebug = 0;
    float curFps = 0.0f;
    SafeRead(flipper + 0x358, curDebug);
    SafeRead(flipper + 0x354, curFps);

    const bool wantedEnabled = enabled_.load(std::memory_order_relaxed);
    const float wantedFps = targetFps_.load(std::memory_order_relaxed);

    if (wantedEnabled) {
        if (curDebug != 1 || std::abs(curFps - wantedFps) > 0.01f) {
            ApplySettings();
        }
    } else {
        if (curDebug != 0) {
            ApplySettings();
        }
    }
}

void FpsUnlockExtension::SetUnlockEnabled(bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_relaxed);
    ApplySettings();
}

bool FpsUnlockExtension::IsUnlockEnabled() const noexcept {
    return enabled_.load(std::memory_order_relaxed);
}

void FpsUnlockExtension::SetTargetFps(float fps) noexcept {
    if (fps < 30.0f) fps = 30.0f;
    targetFps_.store(fps, std::memory_order_relaxed);
    ApplySettings();
}

float FpsUnlockExtension::GetTargetFps() const noexcept {
    return targetFps_.load(std::memory_order_relaxed);
}

void FpsUnlockExtension::SetDisableVsync(bool disable) noexcept {
    disableVsync_.store(disable, std::memory_order_relaxed);
    ApplySettings();
}

bool FpsUnlockExtension::IsDisableVsync() const noexcept {
    return disableVsync_.load(std::memory_order_relaxed);
}

std::shared_ptr<IExtension> CreateFpsUnlockExtension() noexcept {
    return std::make_shared<FpsUnlockExtension>();
}

FpsUnlockExtension* GetFpsUnlockInstance() noexcept {
    return g_fpsUnlockInstance;
}

} // namespace ds3sc::extensions

// C exports
extern "C" {

__declspec(dllexport) void ds3sc_set_fps_unlock(int enabled) {
    auto* ext = ds3sc::extensions::GetFpsUnlockInstance();
    if (ext) ext->SetUnlockEnabled(enabled != 0);
}

__declspec(dllexport) int ds3sc_is_fps_unlock_enabled() {
    auto* ext = ds3sc::extensions::GetFpsUnlockInstance();
    return (ext && ext->IsUnlockEnabled()) ? 1 : 0;
}

__declspec(dllexport) void ds3sc_set_target_fps(float fps) {
    auto* ext = ds3sc::extensions::GetFpsUnlockInstance();
    if (ext) ext->SetTargetFps(fps);
}

__declspec(dllexport) float ds3sc_get_target_fps() {
    auto* ext = ds3sc::extensions::GetFpsUnlockInstance();
    return ext ? ext->GetTargetFps() : 60.0f;
}

__declspec(dllexport) void ds3sc_set_disable_vsync(int disable) {
    auto* ext = ds3sc::extensions::GetFpsUnlockInstance();
    if (ext) ext->SetDisableVsync(disable != 0);
}

__declspec(dllexport) int ds3sc_is_disable_vsync() {
    auto* ext = ds3sc::extensions::GetFpsUnlockInstance();
    return (ext && ext->IsDisableVsync()) ? 1 : 0;
}

__declspec(dllexport) void ds3sc_set_vsync(int enable) {
    auto* ext = ds3sc::extensions::GetFpsUnlockInstance();
    if (ext) ext->SetDisableVsync(enable == 0);
}

__declspec(dllexport) int ds3sc_is_vsync() {
    auto* ext = ds3sc::extensions::GetFpsUnlockInstance();
    return (ext && !ext->IsDisableVsync()) ? 1 : 0;
}

}
