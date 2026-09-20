#pragma once

#include "../extension.h"
#include <memory>
#include <cstdint>
#include <atomic>

namespace ds3sc::extensions {

class CutsceneFixExtension final : public IExtension {
public:
    CutsceneFixExtension() noexcept;
    ~CutsceneFixExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "cutscene_fix"; }
    [[nodiscard]] const char* GetName() const noexcept override {
        return "Cutscene Ally Isolation";
    }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;

    // Status queries
    [[nodiscard]] bool IsInCutscene() const noexcept;
    [[nodiscard]] std::uint32_t GetCutscenesHandled() const noexcept;

private:
    void ProcessCutsceneState() noexcept;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> wasInCutscene_{false};
    std::atomic<std::uint32_t> cutscenesHandled_{0};
};

[[nodiscard]] std::shared_ptr<CutsceneFixExtension> CreateCutsceneFixExtension() noexcept;

} // namespace ds3sc::extensions

extern "C" {
    __declspec(dllexport) bool ds3sc_is_in_cutscene();
    __declspec(dllexport) uint32_t ds3sc_get_cutscenes_handled();
}
