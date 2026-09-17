#include "counters_extension.h"

#include <windows.h>
#include <chrono>
#include <fstream>

namespace ds3sc::extensions {
namespace {

// Memory pointer validation to avoid invalid accesses
inline bool IsValidUserPointer(const void* ptr, std::size_t /*size*/ = sizeof(void*)) noexcept {
    if (!ptr) return false;
    const std::uintptr_t uptr = reinterpret_cast<std::uintptr_t>(ptr);
    if (uptr < 0x10000 || uptr >= 0x7FFFFFFFFFFF) return false;
    return true;
}

template <typename T>
inline bool SafeRead(std::uintptr_t address, T& outValue) noexcept {
    if (!IsValidUserPointer(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    __try {
        outValue = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

CountersExtension* g_pCountersInstance = nullptr;

std::filesystem::path DetermineStatsPath() noexcept {
    // 1. Attempt to locate SeamlessCoop folder containing ds3sc_companion.dll
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&DetermineStatsPath), &hModule)) {
        wchar_t dllPath[MAX_PATH]{};
        if (GetModuleFileNameW(hModule, dllPath, MAX_PATH) > 0) {
            std::filesystem::path dir = std::filesystem::path(dllPath).parent_path();
            if (std::filesystem::is_directory(dir)) {
                return dir / "ds3sc_stats.ini";
            }
        }
    }

    // 2. Fallback to relative SeamplusCoop/ds3sc_stats.ini or SeamlessCoop/ds3sc_stats.ini
    if (std::filesystem::exists("SeamplusCoop")) {
        return "SeamplusCoop/ds3sc_stats.ini";
    }
    if (std::filesystem::exists("SeamlessCoop")) {
        return "SeamlessCoop/ds3sc_stats.ini";
    }

    const std::filesystem::path gamePlus(L"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamplusCoop/ds3sc_stats.ini");
    if (std::filesystem::exists(gamePlus.parent_path())) {
        return gamePlus;
    }
    const std::filesystem::path gameCoop(L"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamlessCoop/ds3sc_stats.ini");
    if (std::filesystem::exists(gameCoop.parent_path())) {
        return gameCoop;
    }

    return "ds3sc_stats.ini";
}

std::filesystem::path DetermineSettingsPath() noexcept {
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&DetermineStatsPath), &hModule)) {
        wchar_t dllPath[MAX_PATH]{};
        if (GetModuleFileNameW(hModule, dllPath, MAX_PATH) > 0) {
            std::filesystem::path dir = std::filesystem::path(dllPath).parent_path();
            std::filesystem::path candidate = dir / "ds3sc_settings.ini";
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
    }

    if (std::filesystem::is_regular_file("SeamplusCoop/ds3sc_settings.ini")) {
        return "SeamplusCoop/ds3sc_settings.ini";
    }
    if (std::filesystem::is_regular_file("SeamlessCoop/ds3sc_settings.ini")) {
        return "SeamlessCoop/ds3sc_settings.ini";
    }

    const std::filesystem::path gamePlus(L"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamplusCoop/ds3sc_settings.ini");
    if (std::filesystem::is_regular_file(gamePlus)) {
        return gamePlus;
    }
    const std::filesystem::path gameCoop(L"C:/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/SeamlessCoop/ds3sc_settings.ini");
    if (std::filesystem::is_regular_file(gameCoop)) {
        return gameCoop;
    }

    return "ds3sc_settings.ini";
}

} // namespace

CountersExtension::CountersExtension() noexcept {
    g_pCountersInstance = this;
}

bool CountersExtension::Initialize() noexcept {
    LoadSettings();
    LoadPersistedStats();

    initialized_.store(true, std::memory_order_release);
    OutputDebugStringA("[ds3sc-counters] Counters module initialized.\n");
    return true;
}

void CountersExtension::Shutdown() noexcept {
    SavePersistedStats(true);
    initialized_.store(false, std::memory_order_release);
    if (g_pCountersInstance == this) {
        g_pCountersInstance = nullptr;
    }
    OutputDebugStringA("[ds3sc-counters] Counters module stopped.\n");
}

CountersSnapshot CountersExtension::GetStats() const noexcept {
    CountersSnapshot snap{};
    snap.deaths = deaths_.load(std::memory_order_relaxed);
    snap.kills = kills_.load(std::memory_order_relaxed);
    snap.backstabsInflicted = backstabsInflicted_.load(std::memory_order_relaxed);
    snap.backstabsReceived = backstabsReceived_.load(std::memory_order_relaxed);
    return snap;
}

void CountersExtension::ResetStats() noexcept {
    deaths_.store(0, std::memory_order_relaxed);
    kills_.store(0, std::memory_order_relaxed);
    backstabsInflicted_.store(0, std::memory_order_relaxed);
    backstabsReceived_.store(0, std::memory_order_relaxed);
    statsDirty_.store(true, std::memory_order_release);
    SavePersistedStats(true);
    OutputDebugStringA("[ds3sc-counters] Counters reset to zero.\n");
}

void CountersExtension::SetOverlayVisible(bool visible) noexcept {
    overlayVisible_.store(visible, std::memory_order_release);
}

bool CountersExtension::IsOverlayVisible() const noexcept {
    return overlayVisible_.load(std::memory_order_acquire);
}

void CountersExtension::ToggleOverlay() noexcept {
    overlayVisible_.store(!overlayVisible_.load(std::memory_order_relaxed), std::memory_order_release);
}

void CountersExtension::RecordDeath() noexcept {
    deaths_.fetch_add(1, std::memory_order_relaxed);
    statsDirty_.store(true, std::memory_order_release);
}

void CountersExtension::RecordKill() noexcept {
    kills_.fetch_add(1, std::memory_order_relaxed);
    statsDirty_.store(true, std::memory_order_release);
}

void CountersExtension::RecordBackstabInflicted() noexcept {
    backstabsInflicted_.fetch_add(1, std::memory_order_relaxed);
    statsDirty_.store(true, std::memory_order_release);
}

void CountersExtension::RecordBackstabReceived() noexcept {
    backstabsReceived_.fetch_add(1, std::memory_order_relaxed);
    statsDirty_.store(true, std::memory_order_release);
}

void CountersExtension::LoadSettings() noexcept {
    const auto ini = DetermineSettingsPath();
    const std::string iniStr = ini.string();

    UINT enabledVal = GetPrivateProfileIntA("COUNTERS", "enabled", 999, iniStr.c_str());
    if (enabledVal == 999) {
        enabledVal = GetPrivateProfileIntA("CONTADORES", "enabled", 999, iniStr.c_str());
    }
    if (enabledVal == 999) {
        enabledVal = GetPrivateProfileIntA("COMBAT_STATS", "enabled", 1, iniStr.c_str());
    }
    enabled_.store(enabledVal != 0, std::memory_order_release);

    UINT showVal = GetPrivateProfileIntA("COUNTERS", "show_overlay", 999, iniStr.c_str());
    if (showVal == 999) {
        showVal = GetPrivateProfileIntA("CONTADORES", "show_overlay", 999, iniStr.c_str());
    }
    if (showVal == 999) {
        showVal = GetPrivateProfileIntA("COMBAT_STATS", "show_overlay", 0, iniStr.c_str());
    }
    if (showVal == 999) {
        showVal = 0;
    }
    overlayVisible_.store(showVal != 0, std::memory_order_release);

    int ox = static_cast<int>(GetPrivateProfileIntA("COUNTERS", "overlay_x", 999, iniStr.c_str()));
    if (ox == 999) {
        ox = static_cast<int>(GetPrivateProfileIntA("CONTADORES", "overlay_x", 999, iniStr.c_str()));
    }
    if (ox == 999) {
        ox = static_cast<int>(GetPrivateProfileIntA("COMBAT_STATS", "overlay_x", 25, iniStr.c_str()));
    }
    overlayX_ = ox;

    int oy = static_cast<int>(GetPrivateProfileIntA("COUNTERS", "overlay_y", 999, iniStr.c_str()));
    if (oy == 999) {
        oy = static_cast<int>(GetPrivateProfileIntA("CONTADORES", "overlay_y", 999, iniStr.c_str()));
    }
    if (oy == 999) {
        oy = static_cast<int>(GetPrivateProfileIntA("COMBAT_STATS", "overlay_y", 60, iniStr.c_str()));
    }
    overlayY_ = oy;

    int tk = static_cast<int>(GetPrivateProfileIntA("COUNTERS", "toggle_key", 999, iniStr.c_str()));
    if (tk == 999) {
        tk = static_cast<int>(GetPrivateProfileIntA("CONTADORES", "toggle_key", 999, iniStr.c_str()));
    }
    if (tk == 999) {
        tk = static_cast<int>(GetPrivateProfileIntA("COMBAT_STATS", "toggle_key", VK_F8, iniStr.c_str()));
    }
    toggleKey_ = tk;

    int rk = static_cast<int>(GetPrivateProfileIntA("COUNTERS", "reset_key", 999, iniStr.c_str()));
    if (rk == 999) {
        rk = static_cast<int>(GetPrivateProfileIntA("CONTADORES", "reset_key", 999, iniStr.c_str()));
    }
    if (rk == 999) {
        rk = static_cast<int>(GetPrivateProfileIntA("COMBAT_STATS", "reset_key", VK_F9, iniStr.c_str()));
    }
    resetKey_ = rk;
}

void CountersExtension::LoadPersistedStats() noexcept {
    statsIniPath_ = DetermineStatsPath();
    const std::string pathStr = statsIniPath_.string();

    if (std::filesystem::is_regular_file(statsIniPath_)) {
        UINT d = GetPrivateProfileIntA("COUNTERS", "deaths", 999999, pathStr.c_str());
        if (d == 999999) {
            d = GetPrivateProfileIntA("CONTADORES", "deaths", 999999, pathStr.c_str());
        }
        if (d == 999999) {
            d = GetPrivateProfileIntA("STATS", "deaths", 0, pathStr.c_str());
        }

        UINT k = GetPrivateProfileIntA("COUNTERS", "kills", 999999, pathStr.c_str());
        if (k == 999999) {
            k = GetPrivateProfileIntA("CONTADORES", "kills", 999999, pathStr.c_str());
        }
        if (k == 999999) {
            k = GetPrivateProfileIntA("STATS", "kills", 0, pathStr.c_str());
        }

        UINT bi = GetPrivateProfileIntA("COUNTERS", "backstabs_inflicted", 999999, pathStr.c_str());
        if (bi == 999999) {
            bi = GetPrivateProfileIntA("CONTADORES", "backstabs_inflicted", 999999, pathStr.c_str());
        }
        if (bi == 999999) {
            bi = GetPrivateProfileIntA("STATS", "backstabs_inflicted", 0, pathStr.c_str());
        }

        UINT br = GetPrivateProfileIntA("COUNTERS", "backstabs_received", 999999, pathStr.c_str());
        if (br == 999999) {
            br = GetPrivateProfileIntA("CONTADORES", "backstabs_received", 999999, pathStr.c_str());
        }
        if (br == 999999) {
            br = GetPrivateProfileIntA("STATS", "backstabs_received", 0, pathStr.c_str());
        }

        deaths_.store(d, std::memory_order_relaxed);
        kills_.store(k, std::memory_order_relaxed);
        backstabsInflicted_.store(bi, std::memory_order_relaxed);
        backstabsReceived_.store(br, std::memory_order_relaxed);
    }
}

void CountersExtension::SavePersistedStats(bool force) noexcept {
    if (!force && !statsDirty_.load(std::memory_order_acquire)) {
        return;
    }

    if (statsIniPath_.empty()) {
        statsIniPath_ = DetermineStatsPath();
    }

    const auto d = deaths_.load(std::memory_order_relaxed);
    const auto k = kills_.load(std::memory_order_relaxed);
    const auto bi = backstabsInflicted_.load(std::memory_order_relaxed);
    const auto br = backstabsReceived_.load(std::memory_order_relaxed);

    std::ofstream file(statsIniPath_, std::ios::out | std::ios::trunc);
    if (file.is_open()) {
        file << "[COUNTERS]\n"
             << "deaths = " << d << "\n"
             << "kills = " << k << "\n"
             << "backstabs_inflicted = " << bi << "\n"
             << "backstabs_received = " << br << "\n";
        file.close();
    }

    statsDirty_.store(false, std::memory_order_release);
}

void CountersExtension::ProcessPlayerDeaths(std::uintptr_t worldChrMan, std::uintptr_t gameDataMan) noexcept {
    // 1. Direct check of local player HP
    std::uintptr_t localPlayerIns = 0;
    if (SafeRead(worldChrMan + 0x80, localPlayerIns) && localPlayerIns) {
        std::uintptr_t modules = 0;
        if ((SafeRead(localPlayerIns + 0x1F90, modules) || SafeRead(localPlayerIns + 0x1F80, modules)) && modules) {
            std::uintptr_t dataModule = 0;
            if (SafeRead(modules + 0x18, dataModule) && dataModule) {
                std::int32_t hp = 0;
                std::int32_t maxHp = 0;
                if (SafeRead(dataModule + 0xD8, hp) && SafeRead(dataModule + 0xDC, maxHp)) {
                    if (maxHp > 0) {
                        if (hp > 0) {
                            playerWasAlive_ = true;
                        } else if (hp <= 0 && playerWasAlive_) {
                            // Live-to-dead transition detected
                            playerWasAlive_ = false;
                            RecordDeath();
                            OutputDebugStringA("[ds3sc-counters] Player death detected via HP = 0.\n");
                        }
                        lastKnownPlayerHp_ = hp;
                    }
                }
            }
        }
    }

    // 2. Correlation with GameDataMan Death Num
    if (gameDataMan) {
        std::uint32_t deaths = 0;
        if (SafeRead(gameDataMan + 0x98, deaths)) {
            if (lastGameDataDeaths_ == 0 && deaths > 0) {
                lastGameDataDeaths_ = deaths;
            } else if (deaths > lastGameDataDeaths_) {
                // If not counted by HP yet, record here
                const std::uint32_t diff = deaths - lastGameDataDeaths_;
                lastGameDataDeaths_ = deaths;
                if (!playerWasAlive_) {
                    // Already counted or synchronized
                } else {
                    for (std::uint32_t i = 0; i < diff; ++i) {
                        RecordDeath();
                    }
                    playerWasAlive_ = false;
                    OutputDebugStringA("[ds3sc-counters] Player death detected via GameDataMan.\n");
                }
            }
        }
    }
}

void CountersExtension::ProcessKills(std::uintptr_t worldChrMan, std::uint64_t nowMs) noexcept {
    // Character group array: WorldChrMan + 0x18 (pointer to list of 64 list pointers)
    std::uintptr_t mapGroupsPtr = 0;
    if (!SafeRead(worldChrMan + 0x18, mapGroupsPtr) || !mapGroupsPtr) {
        return;
    }

    std::uintptr_t localPlayerIns = 0;
    SafeRead(worldChrMan + 0x80, localPlayerIns);

    // Iterate through known map groups (up to 64 groups)
    for (int groupIdx = 0; groupIdx < 64; ++groupIdx) {
        std::uintptr_t groupNode = 0;
        if (!SafeRead(mapGroupsPtr + (groupIdx * sizeof(std::uintptr_t)), groupNode) || !groupNode) {
            continue;
        }

        // Each group node contains list of ChrIns pointers
        std::uintptr_t chrList = 0;
        std::uint32_t chrCount = 0;
        if (!SafeRead(groupNode + 0x00, chrList) || !SafeRead(groupNode + 0x08, chrCount) || !chrList || chrCount > 256) {
            continue;
        }

        for (std::uint32_t i = 0; i < chrCount; ++i) {
            std::uintptr_t chrIns = 0;
            if (!SafeRead(chrList + (i * sizeof(std::uintptr_t)), chrIns) || !chrIns || chrIns == localPlayerIns) {
                continue;
            }

            // Validate health modules
            std::uintptr_t modules = 0;
            if (!SafeRead(chrIns + 0x1F90, modules) && !SafeRead(chrIns + 0x1F80, modules)) {
                continue;
            }
            if (!modules) continue;

            std::uintptr_t dataModule = 0;
            if (!SafeRead(modules + 0x18, dataModule) || !dataModule) {
                continue;
            }

            std::int32_t hp = 0;
            std::int32_t maxHp = 0;
            if (!SafeRead(dataModule + 0xD8, hp) || !SafeRead(dataModule + 0xDC, maxHp)) {
                continue;
            }

            if (maxHp <= 0) {
                continue;
            }

            auto it = trackedEnemies_.find(chrIns);
            if (it == trackedEnemies_.end()) {
                if (hp > 0) {
                    trackedEnemies_[chrIns] = TrackedEnemy{ hp, maxHp, nowMs };
                }
            } else {
                it->second.lastSeenMs = nowMs;
                if (it->second.lastHp > 0 && hp <= 0) {
                    // Live-to-dead transition: confirmed enemy kill
                    RecordKill();
                    it->second.lastHp = 0;
                    OutputDebugStringA("[ds3sc-counters] Enemy kill recorded.\n");
                } else if (hp > 0) {
                    it->second.lastHp = hp;
                }
            }
        }
    }

    // Periodic cleanup every 5 seconds of entities unseen for more than 15 seconds
    if (nowMs - lastCleanupTimeMs_ > 5000) {
        lastCleanupTimeMs_ = nowMs;
        for (auto it = trackedEnemies_.begin(); it != trackedEnemies_.end(); ) {
            if (nowMs > it->second.lastSeenMs && (nowMs - it->second.lastSeenMs) > 15000) {
                it = trackedEnemies_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void CountersExtension::ProcessBackstabs(std::uintptr_t worldChrMan, std::uintptr_t throwMan, std::uint64_t nowMs) noexcept {
    std::uintptr_t localPlayerIns = 0;
    if (!SafeRead(worldChrMan + 0x80, localPlayerIns) || !localPlayerIns) {
        return;
    }

    std::uintptr_t modules = 0;
    if (!SafeRead(localPlayerIns + 0x1F90, modules) && !SafeRead(localPlayerIns + 0x1F80, modules)) {
        return;
    }
    if (!modules) return;

    std::uintptr_t timeActModule = 0;
    if (!SafeRead(modules + 0x10, timeActModule) || !timeActModule) {
        return;
    }

    std::int32_t animId = 0;
    if (!SafeRead(timeActModule + 0xD0, animId)) {
        // Fallback to anim module
        std::uintptr_t animModule = 0;
        if (SafeRead(modules + 0x80, animModule) && animModule) {
            SafeRead(animModule + 0xC8, animId);
        }
    }

    // 1. Attacking Backstab Detection (Inflicted)
    // In DS3, player backstab animations are x500 or x505 (Hornet Ring)
    const bool isBackstabAttackerAnim = (animId > 0) &&
        ((animId % 1000 == 500) || (animId % 1000 == 505));

    // If ThrowMan is available, verify ThrowParamId == 100
    std::uint32_t activeThrowParam = 0;
    if (throwMan) {
        SafeRead(throwMan + 0xE0, activeThrowParam);
    }

    if (isBackstabAttackerAnim || (activeThrowParam == 100 && isBackstabAttackerAnim)) {
        if (!inInflictedBackstab_ && (nowMs - lastInflictedBackstabTime_ > 2500)) {
            inInflictedBackstab_ = true;
            lastInflictedBackstabTime_ = nowMs;
            RecordBackstabInflicted();
            OutputDebugStringA("[ds3sc-counters] Backstab inflicted detected!\n");
        }
    } else {
        if (nowMs - lastInflictedBackstabTime_ > 2500) {
            inInflictedBackstab_ = false;
        }
    }

    // 2. Defending Backstab Detection (Received)
    // Active ThrowParam 100 but player does not have attacker animation and is taking grab damage
    if (activeThrowParam == 100 && !isBackstabAttackerAnim) {
        if (!inReceivedBackstab_ && (nowMs - lastReceivedBackstabTime_ > 2500)) {
            inReceivedBackstab_ = true;
            lastReceivedBackstabTime_ = nowMs;
            RecordBackstabReceived();
            OutputDebugStringA("[ds3sc-counters] Backstab received detected!\n");
        }
    } else {
        if (nowMs - lastReceivedBackstabTime_ > 2500) {
            inReceivedBackstab_ = false;
        }
    }
}

void CountersExtension::OnTick() noexcept {
    if (!initialized_.load(std::memory_order_relaxed)) {
        return;
    }

    // Check keystrokes (F8: Toggle overlay, F9: Reset)
    if (toggleKey_ > 0 && (GetAsyncKeyState(toggleKey_) & 1)) {
        ToggleOverlay();
    }
    if (resetKey_ > 0 && (GetAsyncKeyState(resetKey_) & 1)) {
        ResetStats();
    }

    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto nowMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
    );

    // Limit memory reading to ~30Hz (every 33ms)
    if (nowMs - lastTickTimeMs_ < 33) {
        return;
    }
    lastTickTimeMs_ = nowMs;

    const std::uintptr_t gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) {
        return;
    }

    // Base pointers for Dark Souls III 1.15.2.0
    // WorldChrMan: gameBase + 0x477FDB8
    // GameDataMan: gameBase + 0x47572B8
    // ThrowMan:    gameBase + 0x475A7A8
    std::uintptr_t worldChrMan = 0;
    SafeRead(gameBase + 0x477FDB8, worldChrMan);

    std::uintptr_t gameDataMan = 0;
    SafeRead(gameBase + 0x47572B8, gameDataMan);

    std::uintptr_t throwMan = 0;
    SafeRead(gameBase + 0x475A7A8, throwMan);

    if (worldChrMan) {
        ProcessPlayerDeaths(worldChrMan, gameDataMan);
        if (nowMs - lastKillScanTimeMs_ >= 150) {
            lastKillScanTimeMs_ = nowMs;
            ProcessKills(worldChrMan, nowMs);
        }
        ProcessBackstabs(worldChrMan, throwMan, nowMs);
    }


    // Periodic persistence every 5 seconds if changed
    if (statsDirty_.load(std::memory_order_relaxed) && (nowMs - lastSaveTimeMs_ > 5000)) {
        lastSaveTimeMs_ = nowMs;
        SavePersistedStats(false);
    }
}

std::shared_ptr<IExtension> CreateCountersExtension() noexcept {
    return std::make_shared<CountersExtension>();
}

CountersExtension* GetCountersInstance() noexcept {
    return g_pCountersInstance;
}

} // namespace ds3sc::extensions

// C exports
extern "C" {

void ds3sc_get_counters(uint32_t* deaths, uint32_t* kills,
                        uint32_t* bsInflicted, uint32_t* bsReceived) {
    auto* inst = ds3sc::extensions::GetCountersInstance();
    if (inst) {
        auto s = inst->GetStats();
        if (deaths) *deaths = s.deaths;
        if (kills) *kills = s.kills;
        if (bsInflicted) *bsInflicted = s.backstabsInflicted;
        if (bsReceived) *bsReceived = s.backstabsReceived;
    } else {
        if (deaths) *deaths = 0;
        if (kills) *kills = 0;
        if (bsInflicted) *bsInflicted = 0;
        if (bsReceived) *bsReceived = 0;
    }
}

void ds3sc_reset_counters() {
    auto* inst = ds3sc::extensions::GetCountersInstance();
    if (inst) {
        inst->ResetStats();
    }
}

void ds3sc_set_counters_overlay_visible(int visible) {
    auto* inst = ds3sc::extensions::GetCountersInstance();
    if (inst) {
        inst->SetOverlayVisible(visible != 0);
    }
}

int ds3sc_is_counters_overlay_visible() {
    auto* inst = ds3sc::extensions::GetCountersInstance();
    return (inst && inst->IsOverlayVisible()) ? 1 : 0;
}

// Backward-compatible aliases
void ds3sc_get_contadores(uint32_t* deaths, uint32_t* kills,
                          uint32_t* bsInflicted, uint32_t* bsReceived) {
    ds3sc_get_counters(deaths, kills, bsInflicted, bsReceived);
}

void ds3sc_reset_contadores() {
    ds3sc_reset_counters();
}

void ds3sc_set_contadores_overlay_visible(int visible) {
    ds3sc_set_counters_overlay_visible(visible);
}

int ds3sc_is_contadores_overlay_visible() {
    return ds3sc_is_counters_overlay_visible();
}

void ds3sc_get_combat_stats(uint32_t* deaths, uint32_t* kills,
                            uint32_t* bsInflicted, uint32_t* bsReceived) {
    ds3sc_get_counters(deaths, kills, bsInflicted, bsReceived);
}

void ds3sc_reset_combat_stats() {
    ds3sc_reset_counters();
}

void ds3sc_set_combat_overlay_visible(int visible) {
    ds3sc_set_counters_overlay_visible(visible);
}

int ds3sc_is_combat_overlay_visible() {
    return ds3sc_is_counters_overlay_visible();
}

}
