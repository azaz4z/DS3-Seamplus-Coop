#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

namespace ds3sc::extensions {

class IExtension {
public:
    virtual ~IExtension() = default;

    // Unique module identifier (e.g. "ally_outline", "companion_spawner")
    [[nodiscard]] virtual const char* GetId() const noexcept = 0;

    // Human-readable module name (e.g. "Ally Outline")
    [[nodiscard]] virtual const char* GetName() const noexcept = 0;

    // Module initialization (invoked on worker thread after detecting DS3 and ds3sc.dll)
    virtual bool Initialize() noexcept = 0;

    // Teardown and cleanup when unloading module or exiting game
    virtual void Shutdown() noexcept = 0;

    // Optional periodic tick during update loop
    virtual void OnTick() noexcept {}
};

} // namespace ds3sc::extensions
