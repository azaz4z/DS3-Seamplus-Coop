#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "extension_manager.h"
#if defined(DS3SC_HAS_D3D11_HOOK)
#include "../render/d3d11_hook.h"
#endif

#if defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE
#include "ally_outline/ally_outline_extension.h"
#endif

#if defined(DS3SC_FEATURE_PLAYER_OUTLINE) && DS3SC_FEATURE_PLAYER_OUTLINE
#include "player_outline/player_outline_extension.h"
#endif

#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
#include "ally_markers/ally_markers_extension.h"
#endif

#if defined(DS3SC_FEATURE_COMPANION_SPAWNER) && DS3SC_FEATURE_COMPANION_SPAWNER
#include "companion_spawner/companion_spawner_extension.h"
#endif

#if defined(DS3SC_FEATURE_HIT_SYNC) && DS3SC_FEATURE_HIT_SYNC
#include "hit_sync/hit_sync_extension.h"
#endif

#if (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS) || (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS)
#include "counters/counters_extension.h"
#endif

#if defined(DS3SC_FEATURE_FPS_UNLOCK) && DS3SC_FEATURE_FPS_UNLOCK
#include "fps_unlock/fps_unlock_extension.h"
#endif

#if defined(DS3SC_FEATURE_ANIM_FIX) && DS3SC_FEATURE_ANIM_FIX
#include "anim_fix/anim_fix_extension.h"
#endif

#if defined(DS3SC_FEATURE_CUTSCENE_FIX) && DS3SC_FEATURE_CUTSCENE_FIX
#include "cutscene_fix/cutscene_fix_extension.h"
#endif

#if defined(DS3SC_FEATURE_LAN_COOP) && DS3SC_FEATURE_LAN_COOP
#include "lan_coop/lan_coop_extension.h"
#endif

#if defined(DS3SC_FEATURE_SPECTATOR_FIX) && DS3SC_FEATURE_SPECTATOR_FIX
#include "spectator_fix/spectator_fix_extension.h"
#endif

#if defined(DS3SC_FEATURE_VERBOSE_CONNECTIONS) && DS3SC_FEATURE_VERBOSE_CONNECTIONS
#include "verbose_connections/verbose_connections_extension.h"
#endif




namespace {

DWORD WINAPI ExtensionWorker(void*) {
    // 1. Register activated extensions based on modular compilation
#if defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateAllyOutlineExtension());
#endif

#if defined(DS3SC_FEATURE_PLAYER_OUTLINE) && DS3SC_FEATURE_PLAYER_OUTLINE
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreatePlayerOutlineExtension());
#endif

#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateAllyMarkersExtension());
#endif

#if defined(DS3SC_FEATURE_COMPANION_SPAWNER) && DS3SC_FEATURE_COMPANION_SPAWNER
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateCompanionSpawnerExtension());
#endif

#if defined(DS3SC_FEATURE_HIT_SYNC) && DS3SC_FEATURE_HIT_SYNC
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateHitSyncExtension());
#endif

#if (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS) || (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS)
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateCountersExtension());
#endif

#if defined(DS3SC_FEATURE_FPS_UNLOCK) && DS3SC_FEATURE_FPS_UNLOCK
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateFpsUnlockExtension());
#endif

#if defined(DS3SC_FEATURE_ANIM_FIX) && DS3SC_FEATURE_ANIM_FIX
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateAnimFixExtension());
#endif

#if defined(DS3SC_FEATURE_CUTSCENE_FIX) && DS3SC_FEATURE_CUTSCENE_FIX
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateCutsceneFixExtension());
#endif

#if defined(DS3SC_FEATURE_LAN_COOP) && DS3SC_FEATURE_LAN_COOP
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateLanCoopExtension());
#endif

#if defined(DS3SC_FEATURE_SPECTATOR_FIX) && DS3SC_FEATURE_SPECTATOR_FIX
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateSpectatorFixExtension());
#endif

#if defined(DS3SC_FEATURE_VERBOSE_CONNECTIONS) && DS3SC_FEATURE_VERBOSE_CONNECTIONS
    ds3sc::extensions::ExtensionManager::Instance().Register(
        ds3sc::extensions::CreateVerboseConnectionsExtension());
#endif




    // 2. Wait and initialization loop once DarkSoulsIII.exe and ds3sc.dll are ready
    for (int attempt = 0; attempt < 1000; ++attempt) {
        auto game = GetModuleHandleW(L"DarkSoulsIII.exe");
        auto mod = GetModuleHandleW(L"ds3sc.dll");
        if (game && mod) {
            // Attempt to initialize all modules
            if (ds3sc::extensions::ExtensionManager::Instance().InitializeAll()) {
                // Pin the DLL in memory to prevent accidental unloads while hooks are active
                HMODULE pinned = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   reinterpret_cast<LPCWSTR>(&ExtensionWorker), &pinned);

                // Periodic update loop for extension OnTick() (~30 Hz)
                while (true) {
#if defined(DS3SC_HAS_D3D11_HOOK)
                    ds3sc::render::D3D11HookManager::Instance().MaintainPresentationHooks();
#endif
                    ds3sc::extensions::ExtensionManager::Instance().OnTick();
                    Sleep(33);
                }
                return 0;
            }
        }
        Sleep(25);
    }
    return 1;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE worker = CreateThread(nullptr, 0, ExtensionWorker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    } else if (reason == DLL_PROCESS_DETACH) {
        ds3sc::extensions::ExtensionManager::Instance().ShutdownAll();
    }
    return TRUE;
}
