#pragma once

#include "../extension.h"
#include <memory>
#include <cstdint>
#include <atomic>

namespace ds3sc::extensions {

class SpectatorFixExtension final : public IExtension {
public:
    SpectatorFixExtension() noexcept;
    ~SpectatorFixExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "spectator_fix"; }
    [[nodiscard]] const char* GetName() const noexcept override {
        return "Spectator Stamina Bar & HUD Fix";
    }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;

    // Status queries
    void GetStats(std::uint32_t& staminaFixes,
                  std::uint32_t& spectatorFrames) const noexcept;

private:
    void ProcessSpectatorStaminaFix() noexcept;

    std::atomic<bool> initialized_{false};
    std::atomic<std::uint32_t> staminaFixes_{0};
    std::atomic<std::uint32_t> spectatorFrames_{0};
    bool wasSpectating_{false};
};

[[nodiscard]] std::shared_ptr<SpectatorFixExtension> CreateSpectatorFixExtension() noexcept;

} // namespace ds3sc::extensions

// C export declarations for diagnostics
extern "C" {
    __declspec(dllexport) void ds3sc_get_spectator_fix_stats(uint32_t* staminaFixes, uint32_t* spectatorFrames);
}
