#pragma once

#include "../extension.h"
#include <memory>

namespace ds3sc::extensions {

class AllyOutlineExtension final : public IExtension {
public:
    AllyOutlineExtension() = default;
    ~AllyOutlineExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "ally_outline"; }
    [[nodiscard]] const char* GetName() const noexcept override { return "Ally Outline (D3D11)"; }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override {}
};

std::shared_ptr<IExtension> CreateAllyOutlineExtension() noexcept;

} // namespace ds3sc::extensions
