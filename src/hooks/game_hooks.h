#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

namespace ds3sc::hooks {

enum class Status : int {
    idle = 0,
    waitingForGame = 1,
    unsupportedGame = 2,
    installed = 3,
    failed = 4,
};

struct GameIdentity {
    std::size_t imageSize = 0;
    unsigned long timestamp = 0;
};

// The hooks are deliberately limited to the game's import table. This keeps
// installation reversible and avoids guessing private DS3 instruction layouts.
class GameHooks final {
public:
    bool Install(std::string& error) noexcept;
    void Uninstall() noexcept;
    [[nodiscard]] Status CurrentStatus() const noexcept { return status_.load(); }
    [[nodiscard]] GameIdentity Identity() const noexcept { return identity_; }

private:
    bool InstallImportHook(std::string_view moduleName, std::string_view functionName,
                           void* replacement, void** original, void*** slot,
                           std::string& error) noexcept;
    bool RestoreImportHook(void** slot, void* original) noexcept;

    std::atomic<Status> status_{Status::idle};
    GameIdentity identity_{};
    void** initSlot_ = nullptr;
    void* initOriginal_ = nullptr;
    void** callbacksSlot_ = nullptr;
    void* callbacksOriginal_ = nullptr;
};

}  // namespace ds3sc::hooks
