#include "hit_sync_extension.h"
#include "../../coop/hit_sync.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace ds3sc::extensions {
namespace {

constexpr std::uintptr_t kCandidateWorldChrManRva = 0x477FDB8u;

inline bool IsValidUserPointer(const void* ptr, std::size_t size) noexcept {
    (void)size;
    if (!ptr) return false;
    const auto u = reinterpret_cast<std::uintptr_t>(ptr);
    return (u >= 0x10000ull && u < 0x7fffffffffffull);
}

template <typename T>
inline bool SafeRead(std::uintptr_t address, T& outValue) noexcept {
    if (!IsValidUserPointer(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    __try {
        outValue = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
inline bool SafeWrite(std::uintptr_t address, const T& inValue) noexcept {
    if (!IsValidUserPointer(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    __try {
        *reinterpret_cast<T*>(address) = inValue;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Single global instance for C exports
HitSyncExtension* g_pHitSyncInstance = nullptr;

} // namespace

HitSyncExtension::HitSyncExtension() noexcept {
    g_pHitSyncInstance = this;
}

bool HitSyncExtension::Initialize() noexcept {
    initialized_.store(true, std::memory_order_release);
    OutputDebugStringA("[ds3sc-hit-sync] Hit & damage synchronization module initialized.\n");
    return true;
}

void HitSyncExtension::Shutdown() noexcept {
    initialized_.store(false, std::memory_order_release);
    if (g_pHitSyncInstance == this) {
        g_pHitSyncInstance = nullptr;
    }
    OutputDebugStringA("[ds3sc-hit-sync] Hit synchronization module stopped.\n");
}

void HitSyncExtension::SetEnabled(bool enable) noexcept {
    enabled_.store(enable, std::memory_order_release);
}

bool HitSyncExtension::IsEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

void HitSyncExtension::GetStats(std::uint32_t& hitsRegistered, std::uint32_t& ghostHitsFixed,
                               std::uint32_t& activeEnemies) const noexcept {
    hitsRegistered = hitsRegistered_.load(std::memory_order_relaxed);
    ghostHitsFixed = ghostHitsFixed_.load(std::memory_order_relaxed);
    activeEnemies = activeEnemiesCount_.load(std::memory_order_relaxed);
}

void HitSyncExtension::OnTick() noexcept {
    if (!enabled_.load(std::memory_order_relaxed) ||
        !initialized_.load(std::memory_order_relaxed)) {
        return;
    }

    const std::uint64_t now = GetTickCount64();
    // Run preventive scan every ~150ms (~6.6 Hz) to avoid overloading thread or CPU
    if (now - lastScanTick_ >= 150) {
        lastScanTick_ = now;
        ScanAndFixEnemies();
    }
}

void HitSyncExtension::ScanAndFixEnemies() noexcept {
    const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return;

    std::uintptr_t worldChrMan = 0;
    if (!SafeRead(gameBase + kCandidateWorldChrManRva, worldChrMan) || !worldChrMan) {
        return;
    }

    std::uint32_t enemyCount = 0;

    // Helper lambda function to inspect and fix an individual ChrIns
    auto inspectAndFixChr = [&](std::uintptr_t chrIns) {
        if (!chrIns) return;

        // Check if this is a living enemy
        std::uint32_t charType = 0;
        SafeRead(chrIns + 0x70u, charType);
        // charType 0 = local player, 1/2 = phantoms, 5 = enemy/NPC
        if (charType == 0) return; // Skip local player

        // Obtain Modules pointer (+0x1F90 or fallback +0x1F80)
        std::uintptr_t modules = 0;
        if (!SafeRead(chrIns + 0x1F90u, modules) || !modules) {
            if (!SafeRead(chrIns + 0x1F80u, modules) || !modules) return;
        }

        // Modules + 0x18 -> ChrDataModule
        std::uintptr_t chrData = 0;
        if (!SafeRead(modules + 0x18u, chrData) || !chrData) return;

        // Check current health
        std::int32_t hp = 0;
        std::int32_t maxHp = 0;
        if (!SafeRead(chrData + 0xD8u, hp) || !SafeRead(chrData + 0xDCu, maxHp)) return;
        if (hp <= 0 || maxHp <= 0) return; // Already dead or uninitialized

        // If model is assigned, it is an active entity in the world
        std::uintptr_t model = 0;
        SafeRead(chrIns + 0x48u, model);
        if (!model) return;

        ++enemyCount;

        // 1. Detection and fix of spurious NoDamage flag (ChrData + 0x1C0, bit 1)
        std::uint8_t noDmgByte = 0;
        if (SafeRead(chrData + 0x1C0u, noDmgByte)) {
            constexpr std::uint8_t kNoDamageMask = 0x02; // 1 << 1
            if ((noDmgByte & kNoDamageMask) != 0) {
                // In active combat, an enemy must not have stuck NoDamage
                const std::uint8_t corrected = noDmgByte & ~kNoDamageMask;
                if (SafeWrite(chrData + 0x1C0u, corrected)) {
                    ghostHitsFixed_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // 2. Detection and fix of spurious NoHit flag (ChrIns + 0x1EE8, bit 5)
        std::uint32_t flags = 0;
        if (SafeRead(chrIns + 0x1EE8u, flags)) {
            constexpr std::uint32_t kNoHitMask = 1u << 5; // 0x20
            if ((flags & kNoHitMask) != 0) {
                const std::uint32_t corrected = flags & ~kNoHitMask;
                if (SafeWrite(chrIns + 0x1EE8u, corrected)) {
                    ghostHitsFixed_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    // Iterate through map groups in WorldChrMan (+0x18 / +0x20)
    std::uintptr_t mapGroupsArray = 0;
    if (SafeRead(worldChrMan + 0x18u, mapGroupsArray) && mapGroupsArray != 0) {
        // In DS3, the group structure contains up to 64 groups
        for (std::uint32_t g = 0; g < 64; ++g) {
            std::uintptr_t groupPtr = 0;
            if (!SafeRead(mapGroupsArray + g * sizeof(std::uintptr_t), groupPtr) || !groupPtr) {
                continue;
            }
            // Each group contains an array of ChrIns pointers
            std::uint32_t groupCount = 0;
            std::uintptr_t chrList = 0;
            if (SafeRead(groupPtr + 0x08u, groupCount) && SafeRead(groupPtr + 0x10u, chrList) &&
                groupCount > 0 && groupCount <= 512 && chrList != 0) {
                for (std::uint32_t i = 0; i < groupCount; ++i) {
                    std::uintptr_t enemyChr = 0;
                    if (SafeRead(chrList + i * sizeof(std::uintptr_t), enemyChr) && enemyChr != 0) {
                        inspectAndFixChr(enemyChr);
                    }
                }
            }
        }
    }

    activeEnemiesCount_.store(enemyCount, std::memory_order_relaxed);
}

std::shared_ptr<IExtension> CreateHitSyncExtension() noexcept {
    return std::make_shared<HitSyncExtension>();
}

} // namespace ds3sc::extensions

// C exports for external control, scripts, and live testing
extern "C" {

__declspec(dllexport) void ds3sc_toggle_hit_sync(int enable) {
    if (ds3sc::extensions::g_pHitSyncInstance) {
        ds3sc::extensions::g_pHitSyncInstance->SetEnabled(enable != 0);
    }
}

__declspec(dllexport) int ds3sc_is_hit_sync_active() {
    return (ds3sc::extensions::g_pHitSyncInstance &&
            ds3sc::extensions::g_pHitSyncInstance->IsEnabled()) ? 1 : 0;
}

__declspec(dllexport) void ds3sc_get_hit_sync_stats(uint32_t* hitsRegistered,
                                                   uint32_t* ghostHitsFixed,
                                                   uint32_t* activeEnemies) {
    if (ds3sc::extensions::g_pHitSyncInstance) {
        uint32_t hits = 0, ghosts = 0, enemies = 0;
        ds3sc::extensions::g_pHitSyncInstance->GetStats(hits, ghosts, enemies);
        if (hitsRegistered) *hitsRegistered = hits;
        if (ghostHitsFixed) *ghostHitsFixed = ghosts;
        if (activeEnemies) *activeEnemies = enemies;
    } else {
        if (hitsRegistered) *hitsRegistered = 0;
        if (ghostHitsFixed) *ghostHitsFixed = 0;
        if (activeEnemies) *activeEnemies = 0;
    }
}

}
