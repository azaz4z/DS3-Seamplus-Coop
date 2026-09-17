#pragma once
#include "actor_tracker.h"

namespace ds3sc::render {

// d06f3f..d06f62: ONLY passes 5..8 test DrawEntity+0xa5d and return without
// submitting geometry. Keep this override scoped to one live ally model call;
// the caller serializes ally producers. No visibility groups, transforms,
// material flags, alpha or LOD values are changed.
class ScopedAllyPassVisibility final {
public:
    ScopedAllyPassVisibility(std::uintptr_t entity, std::uint32_t pass) noexcept {
        if (!entity || pass < 5 || pass > 8) return;
        const auto address = entity + 0xa5d;
        if (SafeRead(address, previous_) && previous_ && Write(address, 0)) address_ = address;
    }
    ~ScopedAllyPassVisibility() { if (address_) Write(address_, previous_); }
    ScopedAllyPassVisibility(const ScopedAllyPassVisibility&) = delete;
    ScopedAllyPassVisibility& operator=(const ScopedAllyPassVisibility&) = delete;
    bool Changed() const noexcept { return address_ != 0; }
private:
    static bool Write(std::uintptr_t address, std::uint8_t value) noexcept {
        if (!IsValidUserPointer(reinterpret_cast<void*>(address), 1)) return false;
        __try {
            *reinterpret_cast<volatile std::uint8_t*>(address) = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::uintptr_t address_ = 0;
    std::uint8_t previous_ = 0;
};

// Modifies motor visibility gates on the ally DrawEntity continuously so that
// scene graph traversal and the main camera pass never cull the actor when the
// camera leaves its room / cell portal or moves to a distance.
inline void MakeAllyPersistentlyVisible(std::uintptr_t entity) noexcept {
    if (!entity || !IsValidUserPointer(reinterpret_cast<void*>(entity), 0xc40)) return;
    __try {
        // 1. Draw Groups (+0x88, 32 bytes) -> 0xFF (global entity, belongs to all draw groups)
        auto* const drawGroups = reinterpret_cast<std::uint8_t*>(entity + 0x88);
        for (std::size_t i = 0; i < 32; ++i) {
            drawGroups[i] = 0xFF;
        }

        // 2. Display Groups (+0xa8, 32 bytes) -> 0xFF
        auto* const dispGroups = reinterpret_cast<std::uint8_t*>(entity + 0xa8);
        for (std::size_t i = 0; i < 32; ++i) {
            dispGroups[i] = 0xFF;
        }

        // 3. CPU visibility flags (+0xc3c and +0xbb0) -> bit 0 = 1
        *reinterpret_cast<volatile std::uint8_t*>(entity + 0xc3c) |= 1;
        *reinterpret_cast<volatile std::uint8_t*>(entity + 0xbb0) |= 1;

        // 4. Wall occlusion flag (+0xa5d) is preserved so occlusion culling is accurately tracked
        // (ScopedAllyPassVisibility handles scoped bypass during outline passes 5..8 if needed)

        // 5. Distance fade and LOD fields
        *reinterpret_cast<volatile float*>(entity + 0x170) = 1.0f;
        *reinterpret_cast<volatile float*>(entity + 0x16c) = 1.0f;
        *reinterpret_cast<volatile float*>(entity + 0x168) = 0.0f;
        *reinterpret_cast<volatile std::uint32_t*>(entity + 0x164) =
            *reinterpret_cast<const volatile std::uint32_t*>(entity + 0x160);

        auto* const pLodA = reinterpret_cast<volatile std::int32_t*>(entity + 0x178);
        *pLodA = 0;
        auto* const pLodB = reinterpret_cast<volatile std::int32_t*>(entity + 0x17c);
        *pLodB = 0;
        auto* const pLodA39 = reinterpret_cast<volatile std::int32_t*>(entity + 0x218);
        *pLodA39 = 0;
        auto* const pLodB39 = reinterpret_cast<volatile std::int32_t*>(entity + 0x21c);
        *pLodB39 = 0;

        // 6. Camera pass layer mask (+0x70) -> 0xFFFFFFFF
        *reinterpret_cast<volatile std::uint32_t*>(entity + 0x70) = 0xFFFFFFFF;

        // 7. GXVisbTester (+0x68) -> [tester + 0x40] = 2 (prevent occlusion culling)
        std::uintptr_t tester = 0;
        if (SafeRead(entity + 0x68u, tester) && tester != 0 && IsValidUserPointer(reinterpret_cast<void*>(tester), 0x50)) {
            *reinterpret_cast<volatile std::int32_t*>(tester + 0x40u) = 2;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Complete RAII override during Model() (0xd06e10 / 0xd00200) execution for an ally.
// Guarantees all internal gates evaluate to true (taking the fast path) and restores original values upon exit.
class ScopedAllyModelOverride final {
public:
    ScopedAllyModelOverride(std::uintptr_t entity, std::uint32_t pass) noexcept
        : passVisibility_(entity, pass)
    {
        if (!entity) return;
        entity_ = entity;

        // 1. CPU visibility (+0xc3c for SprjModelDrawEntity, +0xbb0 for SprjAsmModelDrawEntity)
        const auto cpuVisAddr = entity + 0xc3c;
        if (SafeRead(cpuVisAddr, prevCpuVis_) && !(prevCpuVis_ & 1)) {
            if (WriteU8(cpuVisAddr, prevCpuVis_ | 1)) {
                modifiedCpuVis_ = true;
            }
        }
        const auto asmCpuVisAddr = entity + 0xbb0;
        if (SafeRead(asmCpuVisAddr, prevAsmCpuVis_) && !(prevAsmCpuVis_ & 1)) {
            if (WriteU8(asmCpuVisAddr, prevAsmCpuVis_ | 1)) {
                modifiedAsmCpuVis_ = true;
            }
        }

        // 2. Display Groups (+0xa8, 32 bytes) -> 0xFF
        const auto dispGroupsAddr = entity + 0xa8;
        if (SafeReadBytes(dispGroupsAddr, prevDispGroups_.data(), prevDispGroups_.size())) {
            bool allSet = true;
            for (auto b : prevDispGroups_) {
                if (b != 0xFF) { allSet = false; break; }
            }
            if (!allSet) {
                std::array<std::uint8_t, 32> allOnes;
                allOnes.fill(0xFF);
                if (WriteBytes(dispGroupsAddr, allOnes.data(), allOnes.size())) {
                    modifiedDispGroups_ = true;
                }
            }
        }

        // 3. Draw Groups (+0x88, 32 bytes) -> 0xFF
        const auto drawGroupsAddr = entity + 0x88;
        if (SafeReadBytes(drawGroupsAddr, prevDrawGroups_.data(), prevDrawGroups_.size())) {
            bool allSet = true;
            for (auto b : prevDrawGroups_) {
                if (b != 0xFF) { allSet = false; break; }
            }
            if (!allSet) {
                std::array<std::uint8_t, 32> allOnes;
                allOnes.fill(0xFF);
                if (WriteBytes(drawGroupsAddr, allOnes.data(), allOnes.size())) {
                    modifiedDrawGroups_ = true;
                }
            }
        }

        // 4. Distance fade factor (+0x170) -> 1.0f
        const auto fadeAddr = entity + 0x170;
        if (SafeRead(fadeAddr, prevFade_) && prevFade_ != 1.0f) {
            if (WriteFloat(fadeAddr, 1.0f)) {
                modifiedFade_ = true;
            }
        }

        // 5. Fade alt (+0x16c) -> 1.0f
        const auto fadeAltAddr = entity + 0x16c;
        if (SafeRead(fadeAltAddr, prevFadeAlt_) && prevFadeAlt_ != 1.0f) {
            if (WriteFloat(fadeAltAddr, 1.0f)) {
                modifiedFadeAlt_ = true;
            }
        }

        // 6. LOD blend factor (+0x168) -> 0.0f
        const auto lodBlendAddr = entity + 0x168;
        if (SafeRead(lodBlendAddr, prevLodBlend_) && prevLodBlend_ != 0.0f) {
            if (WriteFloat(lodBlendAddr, 0.0f)) {
                modifiedLodBlend_ = true;
            }
        }

        // 7. Make cmp [rbx + 0x160], [rbx + 0x164] equal to skip LOD blend subtraction
        std::uint32_t v160 = 0;
        const auto v164Addr = entity + 0x164;
        if (SafeRead(entity + 0x160, v160) && SafeRead(v164Addr, prevV164_) && prevV164_ != v160) {
            if (WriteU32(v164Addr, v160)) {
                modifiedV164_ = true;
            }
        }

        // 8. Force LOD A (+0x178) and LOD B (+0x17c) to 0 (LOD 0 mesh, full detail, no LOD drop)
        const auto lodAAddr = entity + 0x178;
        if (SafeRead(lodAAddr, prevLodA_) && prevLodA_ != 0) {
            if (WriteI32(lodAAddr, 0)) {
                modifiedLodA_ = true;
            }
        }
        const auto lodBAddr = entity + 0x17c;
        if (SafeRead(lodBAddr, prevLodB_) && prevLodB_ != 0) {
            if (WriteI32(lodBAddr, 0)) {
                modifiedLodB_ = true;
            }
        }

        // 9. Pass 0x39 (57) specific overrides (+0x200..+0x21c)
        if (pass == 0x39) {
            std::uint32_t v200 = 0;
            const auto v204Addr = entity + 0x204;
            if (SafeRead(entity + 0x200, v200) && SafeRead(v204Addr, prevV204_) && prevV204_ != v200) {
                if (WriteU32(v204Addr, v200)) {
                    modifiedV204_ = true;
                }
            }
            const auto lodBlend39Addr = entity + 0x208;
            if (SafeRead(lodBlend39Addr, prevLodBlend39_) && prevLodBlend39_ != 0.0f) {
                if (WriteFloat(lodBlend39Addr, 0.0f)) {
                    modifiedLodBlend39_ = true;
                }
            }
            const auto fade39Addr = entity + 0x20c;
            if (SafeRead(fade39Addr, prevFade39_) && prevFade39_ != 1.0f) {
                if (WriteFloat(fade39Addr, 1.0f)) {
                    modifiedFade39_ = true;
                }
            }
            const auto lodA39Addr = entity + 0x218;
            if (SafeRead(lodA39Addr, prevLodA39_) && prevLodA39_ != 0) {
                if (WriteI32(lodA39Addr, 0)) {
                    modifiedLodA39_ = true;
                }
            }
            const auto lodB39Addr = entity + 0x21c;
            if (SafeRead(lodB39Addr, prevLodB39_) && prevLodB39_ != 0) {
                if (WriteI32(lodB39Addr, 0)) {
                    modifiedLodB39_ = true;
                }
            }
        }

        // 10. Camera pass layer mask (+0x70) -> 0xFFFFFFFF
        const auto layerMaskAddr = entity + 0x70;
        if (SafeRead(layerMaskAddr, prevLayerMask_) && prevLayerMask_ != 0xFFFFFFFF) {
            if (WriteU32(layerMaskAddr, 0xFFFFFFFF)) {
                modifiedLayerMask_ = true;
            }
        }

        // 11. GXVisbTester (+0x68) -> [tester + 0x40] = 2
        std::uintptr_t tester = 0;
        if (SafeRead(entity + 0x68u, tester) && tester != 0 && IsValidUserPointer(reinterpret_cast<void*>(tester), 0x50)) {
            const auto t40Addr = tester + 0x40u;
            if (SafeRead(t40Addr, prevTester40_) && prevTester40_ < 2) {
                if (WriteI32(t40Addr, 2)) {
                    modifiedTester40_ = true;
                    testerAddr_ = t40Addr;
                }
            }
        }
    }

    ~ScopedAllyModelOverride() noexcept {
        if (modifiedTester40_ && testerAddr_) WriteI32(testerAddr_, prevTester40_);
        if (modifiedLayerMask_) WriteU32(entity_ + 0x70, prevLayerMask_);
        if (modifiedLodB39_) WriteI32(entity_ + 0x21c, prevLodB39_);
        if (modifiedLodA39_) WriteI32(entity_ + 0x218, prevLodA39_);
        if (modifiedFade39_) WriteFloat(entity_ + 0x20c, prevFade39_);
        if (modifiedLodBlend39_) WriteFloat(entity_ + 0x208, prevLodBlend39_);
        if (modifiedV204_) WriteU32(entity_ + 0x204, prevV204_);
        if (modifiedLodB_) WriteI32(entity_ + 0x17c, prevLodB_);
        if (modifiedLodA_) WriteI32(entity_ + 0x178, prevLodA_);
        if (modifiedV164_) WriteU32(entity_ + 0x164, prevV164_);
        if (modifiedLodBlend_) WriteFloat(entity_ + 0x168, prevLodBlend_);
        if (modifiedFadeAlt_) WriteFloat(entity_ + 0x16c, prevFadeAlt_);
        if (modifiedFade_) WriteFloat(entity_ + 0x170, prevFade_);
        if (modifiedDrawGroups_) WriteBytes(entity_ + 0x88, prevDrawGroups_.data(), prevDrawGroups_.size());
        if (modifiedDispGroups_) WriteBytes(entity_ + 0xa8, prevDispGroups_.data(), prevDispGroups_.size());
        if (modifiedCpuVis_) WriteU8(entity_ + 0xc3c, prevCpuVis_);
        if (modifiedAsmCpuVis_) WriteU8(entity_ + 0xbb0, prevAsmCpuVis_);
    }

    ScopedAllyModelOverride(const ScopedAllyModelOverride&) = delete;
    ScopedAllyModelOverride& operator=(const ScopedAllyModelOverride&) = delete;

    [[nodiscard]] bool Changed() const noexcept {
        return passVisibility_.Changed() || modifiedCpuVis_ || modifiedAsmCpuVis_ || modifiedDispGroups_ ||
               modifiedDrawGroups_ || modifiedFade_ || modifiedFadeAlt_ || modifiedLodBlend_ ||
               modifiedV164_ || modifiedLodA_ || modifiedLodB_ || modifiedV204_ ||
               modifiedLodBlend39_ || modifiedFade39_ || modifiedLodA39_ || modifiedLodB39_ ||
               modifiedLayerMask_ || modifiedTester40_;
    }

private:
    static bool WriteU8(std::uintptr_t address, std::uint8_t value) noexcept {
        if (!IsValidUserPointer(reinterpret_cast<void*>(address), 1)) return false;
        __try {
            *reinterpret_cast<volatile std::uint8_t*>(address) = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool WriteU32(std::uintptr_t address, std::uint32_t value) noexcept {
        if (!IsValidUserPointer(reinterpret_cast<void*>(address), sizeof(std::uint32_t))) return false;
        __try {
            *reinterpret_cast<volatile std::uint32_t*>(address) = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool WriteI32(std::uintptr_t address, std::int32_t value) noexcept {
        if (!IsValidUserPointer(reinterpret_cast<void*>(address), sizeof(std::int32_t))) return false;
        __try {
            *reinterpret_cast<volatile std::int32_t*>(address) = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool WriteFloat(std::uintptr_t address, float value) noexcept {
        if (!IsValidUserPointer(reinterpret_cast<void*>(address), sizeof(float))) return false;
        __try {
            *reinterpret_cast<volatile float*>(address) = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool WriteBytes(std::uintptr_t address, const void* src, std::size_t size) noexcept {
        if (!src || size == 0 || !IsValidUserPointer(reinterpret_cast<void*>(address), size)) return false;
        __try {
            std::memcpy(reinterpret_cast<void*>(address), src, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    ScopedAllyPassVisibility passVisibility_;
    std::uintptr_t entity_ = 0;
    std::uint8_t prevCpuVis_ = 0;
    std::uint8_t prevAsmCpuVis_ = 0;
    std::array<std::uint8_t, 32> prevDispGroups_{};
    std::array<std::uint8_t, 32> prevDrawGroups_{};
    float prevFade_ = 0.0f;
    float prevFadeAlt_ = 0.0f;
    float prevLodBlend_ = 0.0f;
    std::uint32_t prevV164_ = 0;
    std::int32_t prevLodA_ = 0;
    std::int32_t prevLodB_ = 0;
    std::uint32_t prevV204_ = 0;
    float prevLodBlend39_ = 0.0f;
    float prevFade39_ = 0.0f;
    std::int32_t prevLodA39_ = 0;
    std::int32_t prevLodB39_ = 0;
    std::uint32_t prevLayerMask_ = 0;
    std::int32_t prevTester40_ = 0;
    std::uintptr_t testerAddr_ = 0;

    bool modifiedCpuVis_ = false;
    bool modifiedAsmCpuVis_ = false;
    bool modifiedDispGroups_ = false;
    bool modifiedDrawGroups_ = false;
    bool modifiedFade_ = false;
    bool modifiedFadeAlt_ = false;
    bool modifiedLodBlend_ = false;
    bool modifiedV164_ = false;
    bool modifiedLodA_ = false;
    bool modifiedLodB_ = false;
    bool modifiedV204_ = false;
    bool modifiedLodBlend39_ = false;
    bool modifiedFade39_ = false;
    bool modifiedLodA39_ = false;
    bool modifiedLodB39_ = false;
    bool modifiedLayerMask_ = false;
    bool modifiedTester40_ = false;
};

} // namespace ds3sc::render
