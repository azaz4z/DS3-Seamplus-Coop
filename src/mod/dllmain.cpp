#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "settings.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <exception>
#include <string>

#include "../hooks/game_hooks.h"

namespace {

HMODULE gModule = nullptr;
std::atomic_bool gStarted = false;
// 0 = idle, 1 = loading settings, 2 = settings loaded, 3 = failed.
// None of these states means that cooperative gameplay is implemented.
std::atomic_int gStatus = 0;
ds3sc::hooks::GameHooks gGameHooks;

void LoadPreferences() {
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(gModule, modulePath.data(),
                                            static_cast<DWORD>(modulePath.size()));
    if (!length || length >= modulePath.size()) {
        throw std::runtime_error("Could not obtain full DLL path");
    }

    const auto ini = std::filesystem::path(modulePath.data(), modulePath.data() + length)
                         .parent_path() /
                     "ds3sc_settings.ini";
    const Settings settings = LoadSettings(ini);
    (void)settings;

    OutputDebugStringA("[ds3sc-reconstructed] Preferences loaded.\n");
}

bool InstallHooks() {
    // The injector loads us after the PE image exists, but waiting here also
    // makes direct LoadLibrary use deterministic during early process setup.
    for (int attempt = 0; attempt != 20; ++attempt) {
        if (GetModuleHandleW(L"DarkSoulsIII.exe")) break;
        Sleep(100);
    }
    std::string error;
    if (gGameHooks.Install(error)) {
        OutputDebugStringA("[ds3sc-reconstructed] Steam hooks installed.\n");
        return true;
    }
    const std::string message = "[ds3sc-reconstructed] Hooks not installed: " + error + "\n";
    OutputDebugStringA(message.c_str());
    return false;
}

DWORD WINAPI ModWorker(void* moduleReference) {
    DWORD result = 0;
    try {
        LoadPreferences();
        gStatus = 2;
        const bool hooksInstalled = InstallHooks();
        if (hooksInstalled) return result;
    } catch (const std::exception& error) {
        OutputDebugStringA(error.what());
        gStatus = 3;
        result = 1;
    } catch (...) {
        OutputDebugStringA("[ds3sc-reconstructed] Error loading preferences.\n");
        gStatus = 3;
        result = 1;
    }
    // A successful hook installation keeps the worker's module reference for
    // the lifetime of the game. Unsupported/test loads release that reference.
    FreeLibraryAndExitThread(static_cast<HMODULE>(moduleReference), result);
}

bool EnsureStarted() {
    bool expected = false;
    if (!gStarted.compare_exchange_strong(expected, true)) return true;
    HMODULE workerReference = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&gModule), &workerReference)) {
        gStarted = false;
        gStatus = 3;
        return false;
    }
    gStatus = 1;
    HANDLE thread = CreateThread(nullptr, 0, ModWorker, workerReference, 0, nullptr);
    if (!thread) {
        FreeLibrary(workerReference);
        gStarted = false;
        gStatus = 3;
        return false;
    }
    CloseHandle(thread);
    return true;
}

}  // namespace

extern "C" __declspec(dllexport) int ds3sc_reconstructed_status() noexcept {
    return gStatus.load();
}

extern "C" __declspec(dllexport) int ds3sc_reconstructed_hooks_status() noexcept {
    return static_cast<int>(gGameHooks.CurrentStatus());
}

extern "C" __declspec(dllexport) bool modengine_ext_init(void*, void** extension) noexcept {
    // Original RVA 0x1B90 writes an extension object through argument 2.
    // Returning success without that object is an ABI bug. Until this interface
    // is reconstructed, report unsupported and leave a deterministic output.
    if (extension) *extension = nullptr;
    OutputDebugStringA("[ds3sc-reconstructed] ModEngine ABI not validated for this version.\n");
    return false;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = instance;
        DisableThreadLibraryCalls(instance);
        // The launcher resumes the game immediately after LoadLibrary returns.
        // Install the import hooks before returning so SteamAPI_Init cannot
        // race the worker thread during early game startup.
        std::string hookError;
        if (gGameHooks.Install(hookError)) {
            gStatus = 2;
            OutputDebugStringA("[ds3sc-reconstructed] Steam hooks installed before startup.\n");
        } else if (!hookError.empty()) {
            const std::string message = "[ds3sc-reconstructed] Early hook skipped: " +
                                        hookError + "\n";
            OutputDebugStringA(message.c_str());
        }
        // LoadLibrary-only startup. Never wait for the worker under loader lock.
        EnsureStarted();
    } else if (reason == DLL_PROCESS_DETACH) {
        gGameHooks.Uninstall();
    }
    return TRUE;
}
