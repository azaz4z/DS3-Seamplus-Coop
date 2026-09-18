#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <array>
#include <atomic>
#include <string>
#include <mutex>

namespace ds3sc::render {

// Fast and safe validation of x64 user-space pointers
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

inline bool SafeReadBytes(std::uintptr_t addr, void* dest, std::size_t size) noexcept {
    if (!dest || size == 0) return false;
    if (!IsValidUserPointer(reinterpret_cast<const void*>(addr), size)) return false;
    __try {
        std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct TrackedActor {
    std::uintptr_t chrIns = 0;
    std::uintptr_t chrModel = 0;
    std::uintptr_t drawEntity = 0;
    std::uintptr_t asmEntity = 0;
    std::uintptr_t flverModel = 0;
    std::uintptr_t flverData = 0;
    std::uint32_t meshCount = 0;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float viewPosition[3] = { 0.0f, 0.0f, 0.0f };
    float relPosition[3] = { 0.0f, 0.0f, 0.0f };
    float prevPosition[3] = { 0.0f, 0.0f, 0.0f };
    float velocity[3] = { 0.0f, 0.0f, 0.0f };
    bool isLocal = false;
    bool isAlly = false;
    char nativeType[64] = {};
    std::uint32_t charType = 0;
    std::uint32_t teamType = 0;
    std::uint64_t lastSeenFrame = 0;
};

struct CorrelationResult {
    bool isAllyDraw = false;
    bool isLocalDraw = false;
    std::uintptr_t matchedActor = 0;
    std::uint32_t matchedCbSlot = 0;
    std::size_t matchedCbOffset = 0;
    float deltaDistance = 99999.0f;
};

class ActorTracker final {
public:
    static ActorTracker& Instance() noexcept;

    // Updates the candidate actor list from game memory
    void Update(std::uint64_t frameNumber) noexcept;

    // Updates camera and view matrix from global Constant Buffer (Slot 0)
    void UpdateCameraFromSceneCB(const void* cbData, std::size_t cbSize) noexcept;

    // Active actor control in direct rendering (Option B)
    void SetActiveRenderingActor(std::uintptr_t actor) noexcept {
        activeRenderingActor_ = actor;
    }
    [[nodiscard]] std::uintptr_t GetActiveRenderingActor() const noexcept {
        return activeRenderingActor_;
    }
    void ResetCompanionCache() noexcept {}
    [[nodiscard]] bool IsDrawEntityTrackedAsAlly(std::uintptr_t entity) const noexcept;
    [[nodiscard]] bool IsDrawEntityTrackedAsLocal(std::uintptr_t entity) const noexcept;
    [[nodiscard]] bool IsActorTrackedAsAlly(std::uintptr_t actor) const noexcept;
    [[nodiscard]] bool IsModelTrackedAsAlly(std::uintptr_t model) const noexcept;
    [[nodiscard]] bool IsModelTrackedAsLocal(std::uintptr_t model) const noexcept;
    std::size_t GetFastAllyEntities(std::uintptr_t* outEntities, std::size_t maxCount) const noexcept;
    [[nodiscard]] std::uintptr_t GetFastLocalEntity() const noexcept {
        return fastLocalEntity_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uintptr_t GetFastLocalModel() const noexcept {
        return fastLocalModel_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uintptr_t GetFastLocalAsmEntity() const noexcept {
        return fastLocalAsmEntity_.load(std::memory_order_relaxed);
    }

    // Diagnostic check if a constant buffer contains the position or matrix of a tracked ally
    [[nodiscard]] bool FastCheckAllyMatch(const float* data, std::size_t byteWidth) const noexcept;

    // Checks whether a constant buffer contains coordinates or matrices that
    // uniquely correlate with a tracked actor
    CorrelationResult CheckConstantBufferCorrelation(
        std::uint32_t slot,
        const void* cbData,
        std::size_t cbSize
    ) noexcept;

    static constexpr std::size_t kMaxFastAllies = 32;

    struct FastAllyBounds {
        float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
        float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    };

    [[nodiscard]] bool BoundsContainAlly(const float* bounds) const noexcept;

    struct CameraData {
        float right[3] = { 1.0f, 0.0f, 0.0f };
        float up[3] = { 0.0f, 1.0f, 0.0f };
        float forward[3] = { 0.0f, 0.0f, 1.0f };
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        float fovY = 0.75f;
        float aspect = 1.77778f;
        float nearPlane = 0.08f;
        float farPlane = 10000.0f;
        bool valid = false;
    };

    struct AllyScreenProjection {
        float screenX = 0.0f;
        float screenY = 0.0f;
        float distance = 0.0f;
        bool inFront = false;
    };

    [[nodiscard]] std::size_t GetAllyCount() const noexcept { return allyCount_; }
    [[nodiscard]] bool HasAllies() const noexcept { return allyCount_ > 0; }
    [[nodiscard]] bool HasValidCamera() const noexcept { return hasValidCamera_; }
    [[nodiscard]] const float* GetCameraPosition() const noexcept { return cameraPos_; }
    [[nodiscard]] CameraData GetCameraData() const noexcept;

    bool ProjectWorldToScreen(
        const float worldPos[3],
        float& outScreenX,
        float& outScreenY,
        float& outDistance,
        float screenW,
        float screenH,
        float heightOffset = 2.15f
    ) const noexcept;

    [[nodiscard]] bool IsGameMenuOpen() const noexcept;
    [[nodiscard]] bool IsActorOccluded(const TrackedActor& a) const noexcept;
    [[nodiscard]] bool IsRayOccludedByLocalPlayer(const float targetWorldPos[3]) const noexcept;

    std::size_t GetAllyProjections(
        AllyScreenProjection* outProjections,
        std::size_t maxCount,
        float screenW,
        float screenH,
        bool cullLocalPlayer = true
    ) const noexcept;

private:
    ActorTracker() = default;

    mutable std::recursive_mutex actorsMutex_;
    std::vector<TrackedActor> actors_;
    std::atomic<std::size_t> allyCount_{0};
    std::uint64_t lastLogTime_ = 0;
    std::uintptr_t lastMatchedActor_ = 0;

    // Camera state and view space transformation
    float viewMatrix_[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    float cameraPos_[3] = { 0.0f, 0.0f, 0.0f };
    bool hasValidCamera_ = false;
    CameraData cameraData_{};

    // Actor currently being drawn in engine call (Option B)
    std::uintptr_t activeRenderingActor_ = 0;

    std::atomic<std::size_t> fastAllyCount_{0};
    std::atomic<std::uintptr_t> fastAllyEntities_[kMaxFastAllies]{};
    std::atomic<std::size_t> fastAllyModelCount_{0};
    std::atomic<std::uintptr_t> fastAllyModels_[kMaxFastAllies * 4]{};
    std::atomic<std::uintptr_t> fastLocalEntity_{0};
    std::atomic<std::uintptr_t> fastLocalAsmEntity_{0};
    std::atomic<std::uintptr_t> fastLocalModel_{0};
    std::array<FastAllyBounds, kMaxFastAllies> fastAllyBounds_{};
    std::atomic<std::size_t> fastAllyBoundsCount_{0};

};

} // namespace ds3sc::render