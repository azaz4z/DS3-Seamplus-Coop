#include "spectator_fix_extension.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ds3sc::extensions {
namespace {

constexpr std::uintptr_t kCandidateWorldChrManRva = 0x477FDB8u;

inline bool IsValidUserPointer(const void* ptr, std::size_t size = sizeof(void*)) noexcept {
    if (!ptr) return false;
    const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
    return (addr >= 0x10000u && (addr + size) >= addr && (addr + size) <= 0x7FFFFFFEFFFFu);
}

template<typename T>
inline bool SafeRead(std::uintptr_t addr, T& out) noexcept {
    if (!IsValidUserPointer(reinterpret_cast<const void*>(addr), sizeof(T))) return false;
    __try {
        std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template<typename T>
inline bool SafeWrite(std::uintptr_t addr, const T& val) noexcept {
    if (!IsValidUserPointer(reinterpret_cast<const void*>(addr), sizeof(T))) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(addr), &val, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

SpectatorFixExtension* g_pSpectatorFixInstance = nullptr;

} // namespace

SpectatorFixExtension::SpectatorFixExtension() noexcept {
    g_pSpectatorFixInstance = this;
}

bool SpectatorFixExtension::Initialize() noexcept {
    initialized_.store(true, std::memory_order_release);
    OutputDebugStringA("[ds3sc-spectator-fix] Initialized Spectator Stamina Bar & HUD Fix module.\n");
    return true;
}

void SpectatorFixExtension::Shutdown() noexcept {
    initialized_.store(false, std::memory_order_release);
    OutputDebugStringA("[ds3sc-spectator-fix] Shutdown Spectator Stamina Bar & HUD Fix module.\n");
}

void SpectatorFixExtension::GetStats(std::uint32_t& staminaFixes,
                                     std::uint32_t& spectatorFrames) const noexcept {
    staminaFixes = staminaFixes_.load(std::memory_order_relaxed);
    spectatorFrames = spectatorFrames_.load(std::memory_order_relaxed);
}

void SpectatorFixExtension::OnTick() noexcept {
    if (!initialized_.load(std::memory_order_relaxed)) {
        return;
    }
    ProcessSpectatorStaminaFix();
}

void SpectatorFixExtension::ProcessSpectatorStaminaFix() noexcept {
    const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return;

    std::uintptr_t worldChrMan = 0;
    if (!SafeRead(gameBase + kCandidateWorldChrManRva, worldChrMan) || !worldChrMan) {
        return;
    }

    std::uintptr_t localPlayerIns = 0;
    if (!SafeRead(worldChrMan + 0x80u, localPlayerIns) || !localPlayerIns) {
        return;
    }

    // Read Modules pointer
    std::uintptr_t modules = 0;
    if (!SafeRead(localPlayerIns + 0x1F90u, modules) || !modules) {
        SafeRead(localPlayerIns + 0x1F80u, modules);
    }
    if (!modules) return;

    // Modules + 0x18 -> SprjChrDataModule
    std::uintptr_t chrData = 0;
    if (!SafeRead(modules + 0x18u, chrData) || !chrData) return;

    // Check health
    std::int32_t hp = 0;
    std::int32_t maxHp = 0;
    if (!SafeRead(chrData + 0xD8u, hp) || !SafeRead(chrData + 0xDCu, maxHp)) {
        return;
    }

    if (maxHp <= 0) {
        return; // Character not fully initialized yet
    }

    const bool isDead = (hp <= 0);

    if (!isDead) {
        // Player is alive
        if (wasSpectating_) {
            wasSpectating_ = false;
            OutputDebugStringA("[ds3sc-spectator-fix] Player revived / respawned. Normal stamina restored.\n");
        }
        return;
    }

    // Local player is DEAD: We are in spectator mode / death waiting state
    wasSpectating_ = true;
    spectatorFrames_.fetch_add(1, std::memory_order_relaxed);

    // 1. Sanitize local dead player's stamina attributes
    // Offsets in SprjChrDataModule:
    // +0xF0: Current Stamina (int32)
    // +0xF4: Max Stamina (int32)
    // +0xF8: Base Max Stamina (int32)
    std::int32_t currentSp = 0;
    std::int32_t maxSp = 0;
    std::int32_t baseSp = 0;

    SafeRead(chrData + 0xF0u, currentSp);
    SafeRead(chrData + 0xF4u, maxSp);
    SafeRead(chrData + 0xF8u, baseSp);

    // Ensure Max Stamina is valid (> 0) to completely prevent Scaleform division by zero (NaN)
    if (maxSp <= 0) {
        const std::int32_t safeMax = (baseSp > 0 && baseSp < 1000) ? baseSp : 100;
        if (SafeWrite(chrData + 0xF4u, safeMax)) {
            staminaFixes_.fetch_add(1, std::memory_order_relaxed);
        }
        maxSp = safeMax;
    }

    // Clamp current stamina to exactly 0 while dead:
    // - Prevents negative values (e.g. -40 from lethal hits) being treated as unsigned 4,294,967,256
    // - Prevents overflow past maxSp
    // - Ensures the spectator stamina bar stays stable at 0% without expansion or flickering
    if (currentSp != 0) {
        constexpr std::int32_t zeroSp = 0;
        if (SafeWrite(chrData + 0xF0u, zeroSp)) {
            staminaFixes_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 2. Sanitize all remote allies' SprjChrDataModule in WorldChrMan slots
    // In DS3 netcode, remote players do not sync real-time stamina and may have maxSp = 0.
    // If the spectator camera / HUD evaluates the watched ally, this guarantees it will
    // never hit division-by-zero or corrupted negative stamina.
    std::uint32_t slotCount = 0;
    std::uintptr_t slotsArray = 0;
    if (SafeRead(worldChrMan + 0x38u, slotCount) && SafeRead(worldChrMan + 0x40u, slotsArray) &&
        slotCount <= 32 && slotsArray) {
        for (std::uint32_t i = 0; i < slotCount; ++i) {
            std::uintptr_t allyChr = 0;
            if (!SafeRead(slotsArray + i * 0x38u, allyChr) || !allyChr || allyChr == localPlayerIns) {
                continue;
            }

            std::uintptr_t allyModules = 0;
            if (!SafeRead(allyChr + 0x1F90u, allyModules) || !allyModules) {
                SafeRead(allyChr + 0x1F80u, allyModules);
            }
            if (!allyModules) continue;

            std::uintptr_t allyData = 0;
            if (!SafeRead(allyModules + 0x18u, allyData) || !allyData) continue;

            std::int32_t aMaxSp = 0;
            std::int32_t aBaseSp = 0;
            std::int32_t aSp = 0;
            SafeRead(allyData + 0xF4u, aMaxSp);
            SafeRead(allyData + 0xF8u, aBaseSp);
            SafeRead(allyData + 0xF0u, aSp);

            if (aMaxSp <= 0) {
                const std::int32_t safeAllyMax = (aBaseSp > 0 && aBaseSp < 1000) ? aBaseSp : 100;
                SafeWrite(allyData + 0xF4u, safeAllyMax);
                staminaFixes_.fetch_add(1, std::memory_order_relaxed);
                aMaxSp = safeAllyMax;
            }

            if (aSp < 0 || aSp > aMaxSp) {
                const std::int32_t clampedSp = std::clamp(aSp, 0, aMaxSp);
                SafeWrite(allyData + 0xF0u, clampedSp);
                staminaFixes_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

std::shared_ptr<SpectatorFixExtension> CreateSpectatorFixExtension() noexcept {
    return std::make_shared<SpectatorFixExtension>();
}

} // namespace ds3sc::extensions

extern "C" {
    void ds3sc_get_spectator_fix_stats(uint32_t* staminaFixes, uint32_t* spectatorFrames) {
        if (ds3sc::extensions::g_pSpectatorFixInstance) {
            ds3sc::extensions::g_pSpectatorFixInstance->GetStats(
                *staminaFixes, *spectatorFrames);
        } else {
            if (staminaFixes) *staminaFixes = 0;
            if (spectatorFrames) *spectatorFrames = 0;
        }
    }
}
