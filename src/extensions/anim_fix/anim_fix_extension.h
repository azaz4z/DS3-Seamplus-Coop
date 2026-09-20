#pragma once

#include "../extension.h"
#include <memory>
#include <cstdint>
#include <atomic>

namespace ds3sc::extensions {

class AnimFixExtension final : public IExtension {
public:
    AnimFixExtension() noexcept;
    ~AnimFixExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "anim_fix"; }
    [[nodiscard]] const char* GetName() const noexcept override {
        return "Animation & Locomotion Repair";
    }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;

    // Status queries
    void GetStats(std::uint32_t& animSpeedFixes,
                  std::uint32_t& stuckFlagFixes) const noexcept;

private:
    void ScanAndFixAllyAnimations() noexcept;
    void FixEndingCutsceneSequencer() noexcept;

    std::atomic<bool> initialized_{false};
    std::atomic<std::uint32_t> animSpeedFixes_{0};
    std::atomic<std::uint32_t> stuckFlagFixes_{0};
    std::uint64_t lastAnimScanTick_{0};
};

[[nodiscard]] std::shared_ptr<AnimFixExtension> CreateAnimFixExtension() noexcept;

} // namespace ds3sc::extensions

// Optional C export declarations for diagnostics
extern "C" {
    __declspec(dllexport) void ds3sc_get_anim_fix_stats(uint32_t* animSpeedFixes, uint32_t* stuckFlagFixes);
}
