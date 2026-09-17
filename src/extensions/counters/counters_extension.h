#pragma once

#include "../extension.h"
#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <filesystem>

namespace ds3sc::extensions {

struct CountersSnapshot {
    std::uint32_t deaths = 0;
    std::uint32_t kills = 0;
    std::uint32_t backstabsInflicted = 0;
    std::uint32_t backstabsReceived = 0;
};

using ContadoresSnapshot = CountersSnapshot;
using CombatStatsSnapshot = CountersSnapshot;

class CountersExtension final : public IExtension {
public:
    CountersExtension() noexcept;
    ~CountersExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "counters"; }
    [[nodiscard]] const char* GetName() const noexcept override {
        return "Counters";
    }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;

    // Query and control methods
    [[nodiscard]] CountersSnapshot GetStats() const noexcept;
    void ResetStats() noexcept;
    void SetOverlayVisible(bool visible) noexcept;
    [[nodiscard]] bool IsOverlayVisible() const noexcept;
    void ToggleOverlay() noexcept;

    // Configuration
    [[nodiscard]] int GetOverlayX() const noexcept { return overlayX_; }
    [[nodiscard]] int GetOverlayY() const noexcept { return overlayY_; }

    // Public helper methods for unit tests
    void RecordDeath() noexcept;
    void RecordKill() noexcept;
    void RecordBackstabInflicted() noexcept;
    void RecordBackstabReceived() noexcept;

private:
    void LoadSettings() noexcept;
    void LoadPersistedStats() noexcept;
    void SavePersistedStats(bool force = false) noexcept;

    void ProcessPlayerDeaths(std::uintptr_t worldChrMan, std::uintptr_t gameDataMan) noexcept;
    void ProcessKills(std::uintptr_t worldChrMan, std::uint64_t nowMs) noexcept;
    void ProcessBackstabs(std::uintptr_t worldChrMan, std::uintptr_t throwMan, std::uint64_t nowMs) noexcept;

    // Atomic in-memory counters
    std::atomic<std::uint32_t> deaths_{0};
    std::atomic<std::uint32_t> kills_{0};
    std::atomic<std::uint32_t> backstabsInflicted_{0};
    std::atomic<std::uint32_t> backstabsReceived_{0};

    // Detection state
    bool playerWasAlive_ = false;
    std::int32_t lastKnownPlayerHp_ = 0;
    std::uint32_t lastGameDataDeaths_ = 0;
    std::uint32_t lastGameDataKillBlack_ = 0;

    bool inInflictedBackstab_ = false;
    bool inReceivedBackstab_ = false;
    std::uint64_t lastInflictedBackstabTime_ = 0;
    std::uint64_t lastReceivedBackstabTime_ = 0;

    struct TrackedEnemy {
        std::int32_t lastHp = 0;
        std::int32_t maxHp = 0;
        std::uint64_t lastSeenMs = 0;
    };
    std::unordered_map<std::uintptr_t, TrackedEnemy> trackedEnemies_;

    // Configuration and persistence
    std::atomic<bool> initialized_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> overlayVisible_{false};
    int overlayX_ = 25;
    int overlayY_ = 60;
    int toggleKey_ = VK_F8;
    int resetKey_ = VK_F9;

    std::filesystem::path statsIniPath_;
    std::atomic<bool> statsDirty_{false};
    std::uint64_t lastSaveTimeMs_ = 0;
    std::uint64_t lastTickTimeMs_ = 0;
    std::uint64_t lastKillScanTimeMs_ = 0;
    std::uint64_t lastCleanupTimeMs_ = 0;
};

using ContadoresExtension = CountersExtension;
using CombatStatsExtension = CountersExtension;

std::shared_ptr<IExtension> CreateCountersExtension() noexcept;
inline std::shared_ptr<IExtension> CreateContadoresExtension() noexcept {
    return CreateCountersExtension();
}
inline std::shared_ptr<IExtension> CreateCombatStatsExtension() noexcept {
    return CreateCountersExtension();
}

CountersExtension* GetCountersInstance() noexcept;
inline CountersExtension* GetContadoresInstance() noexcept {
    return GetCountersInstance();
}
inline CountersExtension* GetCombatStatsInstance() noexcept {
    return GetCountersInstance();
}

} // namespace ds3sc::extensions

// C exports for external interoperability
extern "C" {
__declspec(dllexport) void ds3sc_get_counters(uint32_t* deaths, uint32_t* kills,
                                              uint32_t* bsInflicted, uint32_t* bsReceived);
__declspec(dllexport) void ds3sc_reset_counters();
__declspec(dllexport) void ds3sc_set_counters_overlay_visible(int visible);
__declspec(dllexport) int ds3sc_is_counters_overlay_visible();

// Backward-compatible aliases
__declspec(dllexport) void ds3sc_get_contadores(uint32_t* deaths, uint32_t* kills,
                                                uint32_t* bsInflicted, uint32_t* bsReceived);
__declspec(dllexport) void ds3sc_reset_contadores();
__declspec(dllexport) void ds3sc_set_contadores_overlay_visible(int visible);
__declspec(dllexport) int ds3sc_is_contadores_overlay_visible();

__declspec(dllexport) void ds3sc_get_combat_stats(uint32_t* deaths, uint32_t* kills,
                                                  uint32_t* bsInflicted, uint32_t* bsReceived);
__declspec(dllexport) void ds3sc_reset_combat_stats();
__declspec(dllexport) void ds3sc_set_combat_overlay_visible(int visible);
__declspec(dllexport) int ds3sc_is_combat_overlay_visible();
}
