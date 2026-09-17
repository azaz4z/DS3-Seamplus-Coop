#pragma once

#include "../extension.h"
#include <memory>
#include <cstdint>
#include <atomic>

namespace ds3sc::extensions {

class HitSyncExtension final : public IExtension {
public:
    HitSyncExtension() noexcept;
    ~HitSyncExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "hit_sync"; }
    [[nodiscard]] const char* GetName() const noexcept override {
        return "Hit & Damage Synchronization (Hit Registration Fix)";
    }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;

    // Control and statistics
    void SetEnabled(bool enable) noexcept;
    [[nodiscard]] bool IsEnabled() const noexcept;
    void GetStats(std::uint32_t& hitsRegistered, std::uint32_t& ghostHitsFixed,
                  std::uint32_t& activeEnemies) const noexcept;

private:
    void ScanAndFixEnemies() noexcept;

    std::atomic<bool> enabled_{true};
    std::atomic<bool> initialized_{false};
    std::atomic<std::uint32_t> hitsRegistered_{0};
    std::atomic<std::uint32_t> ghostHitsFixed_{0};
    std::atomic<std::uint32_t> activeEnemiesCount_{0};
    std::uint64_t lastScanTick_{0};
};

std::shared_ptr<IExtension> CreateHitSyncExtension() noexcept;

} // namespace ds3sc::extensions
