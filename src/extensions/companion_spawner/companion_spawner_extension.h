#pragma once

#include "../extension.h"
#include <memory>

namespace ds3sc::extensions {

class CompanionSpawnerExtension final : public IExtension {
public:
    CompanionSpawnerExtension() = default;
    ~CompanionSpawnerExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "companion_spawner"; }
    [[nodiscard]] const char* GetName() const noexcept override { return "Companion Spawner (Ash Stone)"; }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override;
};

std::shared_ptr<IExtension> CreateCompanionSpawnerExtension() noexcept;

} // namespace ds3sc::extensions

// Status structure and control commands exported for tests and inspection
extern "C" {
struct CompanionStatus {
    std::uint32_t abi, state, error, giftCount;
    std::uint64_t actor, model, updates, uses, drawEntity;
};
__declspec(dllexport) extern volatile CompanionStatus ds3scCompanionStatus;
__declspec(dllexport) extern volatile LONG ds3scCompanionCommand;
}
