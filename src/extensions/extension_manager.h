#pragma once

#include "extension.h"
#include <vector>
#include <memory>
#include <mutex>

namespace ds3sc::extensions {

class ExtensionManager final {
public:
    static ExtensionManager& Instance() noexcept;

    // Registers a modular extension in the system
    void Register(std::shared_ptr<IExtension> extension) noexcept;

    // Initializes all registered extensions
    bool InitializeAll() noexcept;

    // Stops and cleans up all extensions
    void ShutdownAll() noexcept;

    // Executes periodic tick on all active extensions
    void OnTick() noexcept;

    // Checks whether an extension is registered and active
    [[nodiscard]] bool IsActive(const char* id) const noexcept;

    [[nodiscard]] std::size_t Count() const noexcept { return extensions_.size(); }

private:
    ExtensionManager() = default;
    ~ExtensionManager() = default;

    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IExtension>> extensions_;
    std::vector<std::shared_ptr<IExtension>> active_;
    bool initialized_ = false;
};

} // namespace ds3sc::extensions
