#include "player_outline_extension.h"
#include "../../render/d3d11_hook.h"

#include <cstring>

namespace ds3sc::extensions {

namespace {

void ResolveSettingsPath(char* outPath, std::size_t capacity) noexcept {
    if (!outPath || capacity == 0) return;
    outPath[0] = '\0';

    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CreatePlayerOutlineExtension), &module) && module) {
        GetModuleFileNameA(module, outPath, static_cast<DWORD>(capacity));
        char* slash = std::strrchr(outPath, '\\');
        if (slash) {
            strcpy_s(slash + 1, capacity - static_cast<std::size_t>(slash + 1 - outPath),
                     "ds3sc_settings.ini");
        }
    }

    if (outPath[0] == '\0') {
        if (GetFileAttributesA("SeamplusCoop\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(outPath, capacity, "SeamplusCoop\\ds3sc_settings.ini");
        } else {
            strcpy_s(outPath, capacity, "SeamlessCoop\\ds3sc_settings.ini");
        }
    }
}

} // namespace

bool PlayerOutlineExtension::Initialize() noexcept {
    char iniPath[MAX_PATH] = {};
    ResolveSettingsPath(iniPath, sizeof(iniPath));
    int enabled = GetPrivateProfileIntA("PLAYER_OUTLINE", "enabled", -1, iniPath);
    if (enabled == -1) {
        enabled = GetPrivateProfileIntA("OUTLINE", "outline_local_player", 0, iniPath);
    }
    InterlockedExchange(&ds3scPlayerOutlineEnable, (enabled > 0) ? 1 : 0);
    return render::D3D11HookManager::Instance().Install();
}

void PlayerOutlineExtension::Shutdown() noexcept {
    InterlockedExchange(&ds3scPlayerOutlineEnable, 0);
    render::D3D11HookManager::Instance().Uninstall();
}

std::shared_ptr<IExtension> CreatePlayerOutlineExtension() noexcept {
    return std::make_shared<PlayerOutlineExtension>();
}

} // namespace ds3sc::extensions
