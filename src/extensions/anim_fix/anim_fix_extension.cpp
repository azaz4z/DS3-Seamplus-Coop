#include "anim_fix_extension.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ds3sc::extensions {
namespace {

constexpr std::uintptr_t kCandidateWorldChrManRva = 0x477FDB8u;
constexpr std::uintptr_t kCandidateNewMenuSystemRva = 0x478DA40u;

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

struct CompanionExportStatus {
    std::uint32_t abi, state, error, giftCount;
    std::uint64_t actor, model, updates, uses, drawEntity;
};

bool ReadCompanionStatus(CompanionExportStatus& outStatus) noexcept {
    static const volatile CompanionExportStatus* s_pStatus = nullptr;
    if (!s_pStatus) {
        HMODULE hComp = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ReadCompanionStatus), &hComp);
        if (!hComp) hComp = GetModuleHandleW(L"ds3sc_companion.dll");
        if (!hComp) hComp = GetModuleHandleW(nullptr);
        if (hComp) {
            s_pStatus = reinterpret_cast<const volatile CompanionExportStatus*>(
                GetProcAddress(hComp, "ds3scCompanionStatus"));
        }
    }
    if (!s_pStatus || !IsValidUserPointer(const_cast<const void*>(static_cast<const volatile void*>(s_pStatus)), sizeof(CompanionExportStatus))) {
        return false;
    }
    __try {
        outStatus.abi = s_pStatus->abi;
        outStatus.state = s_pStatus->state;
        outStatus.error = s_pStatus->error;
        outStatus.giftCount = s_pStatus->giftCount;
        outStatus.actor = s_pStatus->actor;
        outStatus.model = s_pStatus->model;
        outStatus.updates = s_pStatus->updates;
        outStatus.uses = s_pStatus->uses;
        outStatus.drawEntity = s_pStatus->drawEntity;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_pStatus = nullptr;
        return false;
    }
}

AnimFixExtension* g_pAnimFixInstance = nullptr;

} // namespace

AnimFixExtension::AnimFixExtension() noexcept {
    g_pAnimFixInstance = this;
}

bool AnimFixExtension::Initialize() noexcept {
    initialized_.store(true, std::memory_order_release);
    OutputDebugStringA("[ds3sc-anim-fix] Initialized native animation & locomotion repair module.\n");
    return true;
}

void AnimFixExtension::Shutdown() noexcept {
    initialized_.store(false, std::memory_order_release);
    OutputDebugStringA("[ds3sc-anim-fix] Shutdown animation & locomotion repair module.\n");
}

void AnimFixExtension::GetStats(std::uint32_t& animSpeedFixes,
                                std::uint32_t& stuckFlagFixes) const noexcept {
    animSpeedFixes = animSpeedFixes_.load(std::memory_order_relaxed);
    stuckFlagFixes = stuckFlagFixes_.load(std::memory_order_relaxed);
}

void AnimFixExtension::OnTick() noexcept {
    if (!initialized_.load(std::memory_order_relaxed)) {
        return;
    }

    const std::uint64_t now = GetTickCount64();

    // Check ending cutscene / event sequence playback speed unfreeze
    FixEndingCutsceneSequencer();

    // Run locomotion animation repair scan every ~100ms (~10 Hz)
    if (now - lastAnimScanTick_ >= 100) {
        lastAnimScanTick_ = now;
        ScanAndFixAllyAnimations();
    }
}

void AnimFixExtension::FixEndingCutsceneSequencer() noexcept {
    const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return;

    // Check if in cutscene / event sequence
    std::uintptr_t menuSystem = 0;
    if (SafeRead(gameBase + kCandidateNewMenuSystemRva, menuSystem) && menuSystem) {
        std::uint8_t cutsceneFlag = 0;
        if (SafeRead(menuSystem + 0x3084u, cutsceneFlag) && cutsceneFlag != 0) {
            // Unfreeze stalled playback speeds on local player / cutscene actors
            std::uintptr_t worldChrMan = 0;
            if (SafeRead(gameBase + kCandidateWorldChrManRva, worldChrMan) && worldChrMan) {
                std::uintptr_t localChr = 0;
                if (SafeRead(worldChrMan + 0x80u, localChr) && localChr) {
                    std::uintptr_t modules = 0;
                    if ((SafeRead(localChr + 0x1F90u, modules) && modules) ||
                        (SafeRead(localChr + 0x1F80u, modules) && modules)) {
                        // ChrBehaviorModule + 0xA58 is AnimationSpeed
                        std::uintptr_t behavior = 0;
                        if (SafeRead(modules + 0x28u, behavior) && behavior) {
                            float animSpeed = 1.0f;
                            if (SafeRead(behavior + 0xA58u, animSpeed)) {
                                if (std::isnan(animSpeed) || animSpeed <= 0.001f) {
                                    constexpr float kDefaultSpeed = 1.0f;
                                    if (SafeWrite(behavior + 0xA58u, kDefaultSpeed)) {
                                        animSpeedFixes_.fetch_add(1, std::memory_order_relaxed);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void AnimFixExtension::ScanAndFixAllyAnimations() noexcept {
    const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return;

    std::uintptr_t worldChrMan = 0;
    if (!SafeRead(gameBase + kCandidateWorldChrManRva, worldChrMan) || !worldChrMan) return;

    auto inspectAndFixAlly = [this](std::uintptr_t chr) {
        if (!chr) return;

        // Modules pointer
        std::uintptr_t modules = 0;
        if (!SafeRead(chr + 0x1F90u, modules) || !modules) {
            SafeRead(chr + 0x1F80u, modules);
        }
        if (!modules) return;

        // 1. BehaviorModule at offset +0x28
        std::uintptr_t behavior = 0;
        if (SafeRead(modules + 0x28u, behavior) && behavior) {
            float animSpeed = 1.0f;
            if (SafeRead(behavior + 0xA58u, animSpeed)) {
                if (std::isnan(animSpeed) || animSpeed <= 0.001f) {
                    // Check if character has moving linear velocity in PhysicsModule (+0x68)
                    std::uintptr_t physics = 0;
                    bool isMoving = false;
                    if (SafeRead(modules + 0x68u, physics) && physics) {
                        float vx = 0.0f, vz = 0.0f;
                        if (SafeRead(physics + 0x70u, vx) && SafeRead(physics + 0x78u, vz)) {
                            const float hSpeedSq = vx * vx + vz * vz;
                            if (hSpeedSq > 0.04f) { // moving faster than 0.2 m/s
                                isMoving = true;
                            }
                        }
                    }

                    // Restore animation playback speed if stuck in sliding/skating state
                    if (isMoving || animSpeed < 0.0f) {
                        constexpr float kNormalSpeed = 1.0f;
                        if (SafeWrite(behavior + 0xA58u, kNormalSpeed)) {
                            animSpeedFixes_.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        }

        // 2. Clear stuck NoMove / transition lock bit (ChrIns + 0x1EE8, bit 7)
        std::uint8_t flags1EE8 = 0;
        if (SafeRead(chr + 0x1EE8u, flags1EE8)) {
            if ((flags1EE8 & 0x80) != 0) { // Bit 7: NoMove / Frozen Locomotion
                // Check if moving
                std::uintptr_t physics = 0;
                if (SafeRead(modules + 0x68u, physics) && physics) {
                    float vx = 0.0f, vz = 0.0f;
                    if (SafeRead(physics + 0x70u, vx) && SafeRead(physics + 0x78u, vz)) {
                        const float hSpeedSq = vx * vx + vz * vz;
                        if (hSpeedSq > 0.04f) {
                            // Clear stuck lock bit
                            flags1EE8 &= 0x7F;
                            if (SafeWrite(chr + 0x1EE8u, flags1EE8)) {
                                stuckFlagFixes_.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
        }
    };

    // Iterate allies in WorldChrMan slots
    std::uint32_t slotCount = 0;
    std::uintptr_t slotsArray = 0;
    if (SafeRead(worldChrMan + 0x38u, slotCount) && SafeRead(worldChrMan + 0x40u, slotsArray) &&
        slotCount <= 32 && slotsArray) {
        for (std::uint32_t i = 0; i < slotCount; ++i) {
            std::uintptr_t chr = 0;
            if (SafeRead(slotsArray + i * 0x38u, chr) && chr) {
                inspectAndFixAlly(chr);
            }
        }
    }

    // Also inspect test companion NPC if present
    CompanionExportStatus comp{};
    if (ReadCompanionStatus(comp) && comp.abi == 1 && comp.state == 3 && comp.actor != 0) {
        inspectAndFixAlly(comp.actor);
    }
}

std::shared_ptr<AnimFixExtension> CreateAnimFixExtension() noexcept {
    return std::make_shared<AnimFixExtension>();
}

} // namespace ds3sc::extensions

extern "C" {

void ds3sc_get_anim_fix_stats(uint32_t* animSpeedFixes, uint32_t* stuckFlagFixes) {
    if (ds3sc::extensions::g_pAnimFixInstance) {
        uint32_t sFix = 0, fFix = 0;
        ds3sc::extensions::g_pAnimFixInstance->GetStats(sFix, fFix);
        if (animSpeedFixes) *animSpeedFixes = sFix;
        if (stuckFlagFixes) *stuckFlagFixes = fFix;
    } else {
        if (animSpeedFixes) *animSpeedFixes = 0;
        if (stuckFlagFixes) *stuckFlagFixes = 0;
    }
}

}
