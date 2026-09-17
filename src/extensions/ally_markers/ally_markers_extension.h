#pragma once

#include "../extension.h"
#include <memory>

namespace ds3sc::extensions {

class AllyMarkersExtension final : public IExtension {
public:
    AllyMarkersExtension() = default;
    ~AllyMarkersExtension() override = default;

    [[nodiscard]] const char* GetId() const noexcept override { return "ally_markers"; }
    [[nodiscard]] const char* GetName() const noexcept override { return "Ally Diamond Markers (D3D11)"; }

    bool Initialize() noexcept override;
    void Shutdown() noexcept override;
    void OnTick() noexcept override {}
};

std::shared_ptr<IExtension> CreateAllyMarkersExtension() noexcept;

} // namespace ds3sc::extensions
