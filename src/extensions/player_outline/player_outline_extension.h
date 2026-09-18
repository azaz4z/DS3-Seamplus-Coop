#pragma once

#include "../extension.h"
#include <memory>

namespace ds3sc::extensions {

class PlayerOutlineExtension final : public IExtension {
public:
    PlayerOutlineExtension() = default;
    ~PlayerOutlineExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "player_outline"; }
    [[nodiscard]] const char* GetName() const noexcept override { return "Player Outline & Silhouette (D3D11)"; }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override {}
};

std::shared_ptr<IExtension> CreatePlayerOutlineExtension() noexcept;

} // namespace ds3sc::extensions
