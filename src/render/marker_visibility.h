#pragma once

#include <cstdint>

namespace ds3sc::render {

// DS3 animation IDs can include a weapon/animation-bank prefix (e.g. 12000000
// for idle). Only these event actions hide markers; attacks/rolls stay visible.
[[nodiscard]] constexpr bool IsMarkerBlockingAnimation(std::int32_t animation) noexcept {
    if (animation < 0) return false;
    switch (animation % 1000000) {
    case 60060: // Traverse fog.
    case 68000: case 68001: case 68002: // Light/kindle bonfire.
    case 68010: case 68011: case 68012: // Sit, rest, stand up.
    case 68100: case 68101: // Bonfire warp.
        return true;
    default:
        return false;
    }
}

// Read-only, shared by both marker render paths through IsGameMenuOpen().
// Offsets target DS3 1.15.2; see analysis/marker-menu-visibility.md for the
// native instruction and live-state evidence. Reader returns false on failure.
template<class Reader>
[[nodiscard]] bool NativeSceneSuppressesMarkers(std::uintptr_t gameBase,
                                                std::uintptr_t localChr,
                                                Reader&& read) noexcept {
    std::uintptr_t menuMan = 0, activeMenu = 0;
    if (read(gameBase + 0x4763258u, menuMan) && menuMan &&
        read(menuMan + 0x1d50u, activeMenu) && activeMenu) return true;

    std::uintptr_t newMenuSystem = 0;
    if (read(gameBase + 0x478da40u, newMenuSystem) && newMenuSystem) {
        std::uint8_t enabled = 1, requestedMode = 3, displayedMode = 3;
        // The frontend publishes mode 3 during gameplay; modes 0/1/2 belong
        // to menus/hidden UI. Check both request and displayed state so opening
        // and closing transitions cannot expose markers for one frame.
        if (read(newMenuSystem + 0x140u, enabled) && enabled == 0) return true;
        if (read(newMenuSystem + 0x143u, requestedMode) && requestedMode < 3) return true;
        if (read(newMenuSystem + 0x2fa1u, displayedMode) && displayedMode < 3) return true;
    }

    std::uintptr_t modules = 0, animationModule = 0;
    std::int32_t animation = -1;
    return localChr && read(localChr + 0x1f90u, modules) && modules &&
        read(modules + 0x80u, animationModule) && animationModule &&
        read(animationModule + 0xc8u, animation) && IsMarkerBlockingAnimation(animation);
}

} // namespace ds3sc::render
