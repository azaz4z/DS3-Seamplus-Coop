#pragma once

#include "../extension.h"
#include <cstdint>
#include <atomic>
#include <memory>
#include <filesystem>

namespace ds3sc::extensions {

class FpsUnlockExtension final : public IExtension {
public:
    FpsUnlockExtension() noexcept;
    ~FpsUnlockExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "fps_unlock"; }
    [[nodiscard]] const char* GetName() const noexcept override { return "FPS Unlocker"; }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;

    // Control and configuration
    void SetUnlockEnabled(bool enabled) noexcept;
    [[nodiscard]] bool IsUnlockEnabled() const noexcept;

    void SetTargetFps(float fps) noexcept;
    [[nodiscard]] float GetTargetFps() const noexcept;

    void SetDisableVsync(bool disable) noexcept;
    [[nodiscard]] bool IsDisableVsync() const noexcept;

    void ApplySettings() noexcept;

private:
    void LoadSettings() noexcept;
    void LocateSprjFlipper() noexcept;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<float> targetFps_{144.0f};
    std::atomic<bool> disableVsync_{false};

    std::uintptr_t sprjFlipperPtrAddress_{0}; // Address of global SprjFlipper* pointer
    std::uint64_t lastTickMs_{0};
    char iniPath_[MAX_PATH]{};
};

std::shared_ptr<IExtension> CreateFpsUnlockExtension() noexcept;
FpsUnlockExtension* GetFpsUnlockInstance() noexcept;

} // namespace ds3sc::extensions

// C exports for external control and test scripts
extern "C" {
__declspec(dllexport) void ds3sc_set_fps_unlock(int enabled);
__declspec(dllexport) int ds3sc_is_fps_unlock_enabled();
__declspec(dllexport) void ds3sc_set_target_fps(float fps);
__declspec(dllexport) float ds3sc_get_target_fps();
__declspec(dllexport) void ds3sc_set_disable_vsync(int disable);
__declspec(dllexport) int ds3sc_is_disable_vsync();
__declspec(dllexport) void ds3sc_set_vsync(int enable);
__declspec(dllexport) int ds3sc_is_vsync();
}
