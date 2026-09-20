#include "actor_tracker.h"
#include "actor_draw_identity.h"
#include "ally_pass_visibility.h"
#include "d3d11_hook.h"
#include "title_menu.h"
#include "marker_visibility.h"

#include <cmath>
#include <cstdio>
#include <algorithm>

#if defined(DS3SC_STANDALONE_TEST) && !defined(DS3SC_HAS_D3D11_HOOK)
extern "C" volatile LONG ds3scPlayerOutlineEnable = 0;
#endif

namespace ds3sc::render {
namespace {


constexpr std::uintptr_t kCandidateWorldChrManRva = 0x477FDB8u;
constexpr std::uintptr_t kCandidateFieldAreaRva = 0x475ABD0u;
constexpr std::uintptr_t kCandidateNewMenuSystemRva = 0x478DA40u;

struct CompanionExportStatus {
    std::uint32_t abi, state, error, giftCount;
    std::uint64_t actor, model, updates, uses, drawEntity;
};

// Attempts to read companion module status if loaded
bool ReadCompanionStatus(CompanionExportStatus& outStatus) noexcept {
    static const volatile CompanionExportStatus* s_pStatus = nullptr;
    if (!s_pStatus) {
        HMODULE hComp = nullptr;
        // First query the module handle containing this function itself
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ReadCompanionStatus), &hComp);
        if (!hComp) {
            hComp = GetModuleHandleW(L"ds3sc_companion.dll");
        }
        if (!hComp) {
            hComp = GetModuleHandleW(nullptr);
        }
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

// Safe read of a ChrIns position through its physics module
bool TryReadActorPosition(std::uintptr_t chrIns, float outPos[3]) noexcept {
    if (!chrIns) return false;
    std::uintptr_t modules = 0;
    // Verified offset in analysis: ChrIns + 0x1F90 (Modules)
    if (!SafeRead(chrIns + 0x1F90u, modules) || !modules) {
        // Fallback to alternative offset 0x1F80 in older versions
        if (!SafeRead(chrIns + 0x1F80u, modules) || !modules) return false;
    }

    std::uintptr_t physics = 0;
    // Modules + 0x68 -> Physics
    if (!SafeRead(modules + 0x68u, physics) || !physics) return false;

    // Physics + 0x80 -> float[3] position (X, Y, Z)
    float rawPos[3] = { 0.0f, 0.0f, 0.0f };
    if (!SafeReadBytes(physics + 0x80u, rawPos, sizeof(rawPos))) return false;

    if (!std::isfinite(rawPos[0]) || !std::isfinite(rawPos[1]) || !std::isfinite(rawPos[2])) {
        return false;
    }

    outPos[0] = rawPos[0];
    outPos[1] = rawPos[1];
    outPos[2] = rawPos[2];
    return true;
}

// Safe read of a ChrModel world transform row-major affine 3x4 (floats 3, 7, 11 are X, Y, Z)
bool TryReadActorPositionFromModel(std::uintptr_t chrModel, float outPos[3]) noexcept {
    if (!chrModel) return false;
    float world[12]{};
    if (!SafeReadBytes(chrModel + 0x20u, world, sizeof(world))) return false;
    if (!std::isfinite(world[3]) || !std::isfinite(world[7]) || !std::isfinite(world[11])) return false;
    if (std::abs(world[3]) < 0.001f && std::abs(world[7]) < 0.001f && std::abs(world[11]) < 0.001f) return false;
    outPos[0] = world[3];
    outPos[1] = world[7];
    outPos[2] = world[11];
    return true;
}

// Attempts to read camera and its orthonormal basis from FieldArea
bool TryReadCameraFromFieldArea(std::uintptr_t gameBase, ActorTracker::CameraData& outCam) noexcept {
    if (!gameBase) return false;
    std::uintptr_t fieldArea = 0;
    if (!SafeRead(gameBase + kCandidateFieldAreaRva, fieldArea) || !fieldArea) return false;

    // FieldArea + 0x28 -> chrCam (fallback to +0x18)
    std::uintptr_t chrCam = 0;
    if (!SafeRead(fieldArea + 0x28u, chrCam) || !chrCam) {
        SafeRead(fieldArea + 0x18u, chrCam);
    }
    if (!chrCam) return false;

    // chrCam + 0x60 -> followCam (fallback to +0xE8)
    std::uintptr_t followCam = 0;
    if (!SafeRead(chrCam + 0x60u, followCam) || !followCam) {
        SafeRead(chrCam + 0xE8u, followCam);
    }
    if (!followCam) return false;

    // FollowCam structure:
    // +0x10: Right vector (float[3])
    // +0x20: Up vector (float[3])
    // +0x30: Forward vector (float[3])
    // +0x40: Camera Position (float[3])
    // +0x50: FovY (float)
    // +0x54: Aspect ratio (float)
    // +0x58: Near plane (float)
    // +0x5C: Far plane (float)
    float rawData[20]{};
    if (!SafeReadBytes(followCam + 0x10u, rawData, sizeof(rawData))) return false;

    const float rLenSq = rawData[0] * rawData[0] + rawData[1] * rawData[1] + rawData[2] * rawData[2];
    const float uLenSq = rawData[4] * rawData[4] + rawData[5] * rawData[5] + rawData[6] * rawData[6];
    const float fLenSq = rawData[8] * rawData[8] + rawData[9] * rawData[9] + rawData[10] * rawData[10];

    const bool basisValid = (std::abs(rLenSq - 1.0f) < 0.25f &&
                             std::abs(uLenSq - 1.0f) < 0.25f &&
                             std::abs(fLenSq - 1.0f) < 0.25f);

    // Read camera position from available sources:
    // 1. FollowCam + 0x40
    float camPos[3] = { 0.0f, 0.0f, 0.0f };
    bool hasPos = false;
    if (std::isfinite(rawData[12]) && std::isfinite(rawData[13]) && std::isfinite(rawData[14]) &&
        (std::abs(rawData[12]) > 0.01f || std::abs(rawData[13]) > 0.01f || std::abs(rawData[14]) > 0.01f)) {
        camPos[0] = rawData[12];
        camPos[1] = rawData[13];
        camPos[2] = rawData[14];
        hasPos = true;
    }

    // 2. Fallback to FollowCam + 0x170 (ChrOrgOffset)
    if (!hasPos) {
        float rawCam[3]{};
        if (SafeReadBytes(followCam + 0x170u, rawCam, sizeof(rawCam))) {
            if (std::isfinite(rawCam[0]) && std::isfinite(rawCam[1]) && std::isfinite(rawCam[2]) &&
                (std::abs(rawCam[0]) > 0.001f || std::abs(rawCam[1]) > 0.001f || std::abs(rawCam[2]) > 0.001f)) {
                camPos[0] = rawCam[0];
                camPos[1] = rawCam[1];
                camPos[2] = rawCam[2];
                hasPos = true;
            }
        }
    }

    // 3. Fallback to FieldArea + 0x18 -> +0xE8 -> +0x40
    if (!hasPos) {
        std::uintptr_t p18 = 0, pE8 = 0;
        if (SafeRead(fieldArea + 0x18u, p18) && p18 &&
            SafeRead(p18 + 0xE8u, pE8) && pE8) {
            float altCam[3]{};
            if (SafeReadBytes(pE8 + 0x40u, altCam, sizeof(altCam))) {
                if (std::isfinite(altCam[0]) && std::isfinite(altCam[1]) && std::isfinite(altCam[2]) &&
                    (std::abs(altCam[0]) > 0.001f || std::abs(altCam[1]) > 0.001f || std::abs(altCam[2]) > 0.001f)) {
                    camPos[0] = altCam[0];
                    camPos[1] = altCam[2];
                    camPos[2] = altCam[1];
                    hasPos = true;
                }
            }
        }
    }

    if (!hasPos) return false;

    float fovY = rawData[16];
    if (fovY <= 0.1f || fovY >= 3.0f) fovY = 0.785398f;
    float aspect = rawData[17];
    if (aspect <= 0.5f || aspect >= 4.0f) aspect = 16.0f / 9.0f;
    float nearPlane = (rawData[18] > 0.001f) ? rawData[18] : 0.08f;
    float farPlane = (rawData[19] > 10.0f) ? rawData[19] : 10000.0f;

    outCam.pos[0] = camPos[0];
    outCam.pos[1] = camPos[1];
    outCam.pos[2] = camPos[2];
    outCam.fovY = fovY;
    outCam.aspect = aspect;
    outCam.nearPlane = nearPlane;
    outCam.farPlane = farPlane;

    if (basisValid) {
        outCam.right[0] = rawData[0];   outCam.right[1] = rawData[1];   outCam.right[2] = rawData[2];
        outCam.up[0] = rawData[4];      outCam.up[1] = rawData[5];      outCam.up[2] = rawData[6];
        outCam.forward[0] = rawData[8]; outCam.forward[1] = rawData[9]; outCam.forward[2] = rawData[10];
        outCam.valid = true;
    } else {
        outCam.valid = false;
    }

    return true;
}

} // namespace

ActorTracker& ActorTracker::Instance() noexcept {
    static ActorTracker tracker;
    return tracker;
}

bool ActorTracker::IsDrawEntityTrackedAsLocal(std::uintptr_t entity) const noexcept {
    if (!entity) return false;
    const auto draw = fastLocalEntity_.load(std::memory_order_relaxed);
    if (draw && (entity == draw || (entity >= 0xd0u && (entity - 0xd0u) == draw))) return true;
    const auto asmDraw = fastLocalAsmEntity_.load(std::memory_order_relaxed);
    if (asmDraw) {
        if (entity == asmDraw || (entity >= 0xd0u && (entity - 0xd0u) == asmDraw)) return true;
        std::uintptr_t parentAsm = 0;
        if (SafeRead(entity + 0xe8u, parentAsm) && parentAsm == asmDraw) return true;
        if (entity >= 0xd0u && SafeRead(entity - 0xd0u + 0xe8u, parentAsm) && parentAsm == asmDraw) return true;
    }
    const auto model = fastLocalModel_.load(std::memory_order_relaxed);
    if (model && entity == model) return true;
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    for (const auto& actor : actors_) {
        if (actor.isLocal) {
            if (actor.drawEntity && (entity == actor.drawEntity || (entity >= 0xd0u && (entity - 0xd0u) == actor.drawEntity))) return true;
            if (actor.asmEntity) {
                if (entity == actor.asmEntity || (entity >= 0xd0u && (entity - 0xd0u) == actor.asmEntity)) return true;
                std::uintptr_t parentAsm = 0;
                if (SafeRead(entity + 0xe8u, parentAsm) && parentAsm == actor.asmEntity) return true;
                if (entity >= 0xd0u && SafeRead(entity - 0xd0u + 0xe8u, parentAsm) && parentAsm == actor.asmEntity) return true;
            }
            if (actor.chrModel && entity == actor.chrModel) return true;
        }
    }
    return false;
}

bool ActorTracker::IsModelTrackedAsLocal(std::uintptr_t model) const noexcept {
    if (!model) return false;
    const auto localModel = fastLocalModel_.load(std::memory_order_relaxed);
    if (localModel && model == localModel) return true;
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    for (const auto& actor : actors_) {
        if (actor.isLocal && actor.chrModel && model == actor.chrModel) return true;
    }
    return false;
}

bool ActorTracker::IsDrawEntityTrackedAsAlly(std::uintptr_t entity) const noexcept {
    if (!entity) return false;
    if (IsDrawEntityTrackedAsLocal(entity)) return false;
    const std::size_t fastCount = fastAllyCount_.load(std::memory_order_acquire);
    if (fastCount == 0) return false;

    for (std::size_t i = 0; i < fastCount && i < kMaxFastAllies; ++i) {
        const auto fastEnt = fastAllyEntities_[i].load(std::memory_order_relaxed);
        if (fastEnt) {
            if (entity == fastEnt || (entity >= 0xd0u && (entity - 0xd0u) == fastEnt)) {
                return true;
            }
            std::uintptr_t parentAsm = 0;
            if (SafeRead(entity + 0xe8u, parentAsm) && parentAsm == fastEnt) return true;
            if (entity >= 0xd0u && SafeRead(entity - 0xd0u + 0xe8u, parentAsm) && parentAsm == fastEnt) return true;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    for (const auto& actor : actors_) {
        if (actor.isAlly) {
            if (actor.drawEntity && (entity == actor.drawEntity || (entity >= 0xd0u && (entity - 0xd0u) == actor.drawEntity))) return true;
            if (actor.asmEntity) {
                if (entity == actor.asmEntity || (entity >= 0xd0u && (entity - 0xd0u) == actor.asmEntity)) return true;
                std::uintptr_t parentAsm = 0;
                if (SafeRead(entity + 0xe8u, parentAsm) && parentAsm == actor.asmEntity) return true;
                if (entity >= 0xd0u && SafeRead(entity - 0xd0u + 0xe8u, parentAsm) && parentAsm == actor.asmEntity) return true;
            }
            if (actor.chrModel && entity == actor.chrModel) return true;
        }
    }
    return false;
}

std::size_t ActorTracker::GetFastAllyEntities(std::uintptr_t* outEntities, std::size_t maxCount) const noexcept {
    if (!outEntities || maxCount == 0) return 0;
    const std::size_t count = fastAllyCount_.load(std::memory_order_acquire);
    const std::size_t toCopy = (count < maxCount) ? count : maxCount;
    for (std::size_t i = 0; i < toCopy; ++i) {
        outEntities[i] = fastAllyEntities_[i].load(std::memory_order_relaxed);
    }
    return toCopy;
}

bool ActorTracker::IsModelTrackedAsAlly(std::uintptr_t model) const noexcept {
    if (!model) return false;
    if (IsModelTrackedAsLocal(model)) return false;
    const std::size_t fastCount = fastAllyModelCount_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < fastCount && i < kMaxFastAllies * 4; ++i) {
        if (fastAllyModels_[i].load(std::memory_order_relaxed) == model) return true;
    }
    CompanionExportStatus comp{};
    if (ReadCompanionStatus(comp) && comp.abi == 1 && comp.state == 3 && comp.actor != 0) {
        if (comp.model != 0 && comp.model == model) return true;
    }
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    for (const auto& a : actors_) {
        if (a.isAlly && a.chrModel && a.chrModel == model) {
            return true;
        }
    }
    return false;
}

bool ActorTracker::IsActorTrackedAsAlly(std::uintptr_t actor) const noexcept {
    if (!actor) return false;
    CompanionExportStatus comp{};
    if (ReadCompanionStatus(comp) && comp.abi == 1 && comp.state == 3 && comp.actor != 0) {
        if (comp.actor == actor || comp.model == actor || comp.drawEntity == actor) return true;
        if (comp.model != 0) {
            std::uintptr_t draw = 0;
            if (SafeRead(comp.model + 0x8u, draw) && draw == actor) return true;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    for (const auto& a : actors_) {
        if (a.isAlly) {
            if (a.chrIns == actor || a.chrModel == actor || a.drawEntity == actor) {
                return true;
            }
        }
    }
    return false;
}

void ActorTracker::UpdateCameraFromSceneCB(const void* cbData, std::size_t cbSize) noexcept {
    if (!cbData || cbSize < 64) return;
    const auto floats = reinterpret_cast<const float*>(cbData);

    // Verify if the first 16 floats form an orthonormal view matrix
    // R0^2 + R1^2 + R2^2 ≈ 1.0
    const float len0 = floats[0] * floats[0] + floats[1] * floats[1] + floats[2] * floats[2];
    const float len1 = floats[4] * floats[4] + floats[5] * floats[5] + floats[6] * floats[6];
    const float len2 = floats[8] * floats[8] + floats[9] * floats[9] + floats[10] * floats[10];

    if (std::abs(len0 - 1.0f) < 0.15f && std::abs(len1 - 1.0f) < 0.15f && std::abs(len2 - 1.0f) < 0.15f) {
        std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
        std::memcpy(viewMatrix_, floats, sizeof(viewMatrix_));

        // In row-major view matrix: C = -T * R^T
        // T = [floats[12], floats[13], floats[14]]
        const float tx = floats[12], ty = floats[13], tz = floats[14];
        cameraPos_[0] = -(tx * floats[0] + ty * floats[4] + tz * floats[8]);
        cameraPos_[1] = -(tx * floats[1] + ty * floats[5] + tz * floats[9]);
        cameraPos_[2] = -(tx * floats[2] + ty * floats[6] + tz * floats[10]);

        if (std::isfinite(cameraPos_[0]) && std::isfinite(cameraPos_[1]) && std::isfinite(cameraPos_[2])) {
            hasValidCamera_ = true;
            for (auto& a : actors_) {
                a.relPosition[0] = a.position[0] - cameraPos_[0];
                a.relPosition[1] = a.position[1] - cameraPos_[1];
                a.relPosition[2] = a.position[2] - cameraPos_[2];

                a.viewPosition[0] = a.position[0] * viewMatrix_[0] + a.position[1] * viewMatrix_[4] + a.position[2] * viewMatrix_[8] + viewMatrix_[12];
                a.viewPosition[1] = a.position[0] * viewMatrix_[1] + a.position[1] * viewMatrix_[5] + a.position[2] * viewMatrix_[9] + viewMatrix_[13];
                a.viewPosition[2] = a.position[0] * viewMatrix_[2] + a.position[1] * viewMatrix_[6] + a.position[2] * viewMatrix_[10] + viewMatrix_[14];
            }
        }
    }
}

void ActorTracker::Update(std::uint64_t frameNumber) noexcept {
    if (!settingsLoaded_) {
        LoadSettingsFromIni();
    }

    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    actors_.clear();
    hasValidCamera_ = false;
    allyCount_ = 0;

    auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"DarkSoulsIII.exe"));
    if (!gameBase) gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return;

    // Update camera position and orthonormal basis if FieldArea is available
    CameraData cam{};
    if (TryReadCameraFromFieldArea(gameBase, cam)) {
        cameraData_ = cam;
        cameraPos_[0] = cam.pos[0];
        cameraPos_[1] = cam.pos[1];
        cameraPos_[2] = cam.pos[2];
        hasValidCamera_ = true;
    }

    // 1. Read candidate WorldChrManImp and local player first
    std::uintptr_t worldChrMan = 0;
    std::uintptr_t localChr = 0;
    if (SafeRead(gameBase + kCandidateWorldChrManRva, worldChrMan) && worldChrMan != 0) {
        SafeRead(worldChrMan + 0x80u, localChr);
    }
    if (worldChrMan == 0 || localChr == 0) {
        fastLocalEntity_.store(0, std::memory_order_relaxed);
        fastLocalAsmEntity_.store(0, std::memory_order_relaxed);
        fastLocalModel_.store(0, std::memory_order_relaxed);
    }

    // 2. Check local companion test dummy (ds3sc_companion)
    CompanionExportStatus compStatus{};
    if (ReadCompanionStatus(compStatus) && compStatus.abi == 1 && compStatus.state == 3 &&
        compStatus.actor != 0 && compStatus.actor != localChr) {
        TrackedActor compActor{};
        compActor.chrIns = compStatus.actor;
        compActor.chrModel = compStatus.model;
        if (compActor.chrModel == 0 && compActor.chrIns != 0) {
            SafeRead(compActor.chrIns + 0x48u, compActor.chrModel);
        }
        if (compActor.drawEntity == 0 && compStatus.drawEntity != 0) {
            compActor.drawEntity = compStatus.drawEntity;
        }
        if (compActor.chrModel != 0) {
            if (compActor.drawEntity == 0) {
                SafeRead(compActor.chrModel + 0x8u, compActor.drawEntity);
            }
            if (compActor.drawEntity != 0) {
                SafeRead(compActor.drawEntity + 0x20u, compActor.meshCount);
                SafeRead(compActor.drawEntity + 0xe8u, compActor.flverModel);
                if (compActor.flverModel != 0) {
                    SafeRead(compActor.flverModel + 0x8u, compActor.flverData);
                }
                if (!compActor.flverData) {
                    SafeRead(compActor.drawEntity + 0xb00u, compActor.flverData);
                }
            }
            std::uintptr_t p20b0 = 0;
            if (SafeRead(compActor.chrIns + 0x20b0u, p20b0) && p20b0 != 0) {
                SafeRead(p20b0 + 0x8u, compActor.asmEntity);
            }
            if (!IsValidUserPointer(reinterpret_cast<void*>(compActor.asmEntity), sizeof(std::uintptr_t))) {
                compActor.asmEntity = 0;
            }
        }
        compActor.isAlly = true;
        compActor.isLocal = false;
        compActor.lastSeenFrame = frameNumber;
        std::snprintf(compActor.nativeType, sizeof(compActor.nativeType), "CompanionDummy");

        bool hasCompPos = TryReadActorPosition(compActor.chrIns, compActor.position);
        if (!hasCompPos && compActor.chrModel) {
            hasCompPos = TryReadActorPositionFromModel(compActor.chrModel, compActor.position);
        }
        if (hasCompPos) {
            if (hasValidCamera_) {
                compActor.relPosition[0] = compActor.position[0] - cameraPos_[0];
                compActor.relPosition[1] = compActor.position[1] - cameraPos_[1];
                compActor.relPosition[2] = compActor.position[2] - cameraPos_[2];

                compActor.viewPosition[0] = compActor.position[0] * viewMatrix_[0] + compActor.position[1] * viewMatrix_[4] + compActor.position[2] * viewMatrix_[8] + viewMatrix_[12];
                compActor.viewPosition[1] = compActor.position[0] * viewMatrix_[1] + compActor.position[1] * viewMatrix_[5] + compActor.position[2] * viewMatrix_[9] + viewMatrix_[13];
                compActor.viewPosition[2] = compActor.position[0] * viewMatrix_[2] + compActor.position[1] * viewMatrix_[6] + compActor.position[2] * viewMatrix_[10] + viewMatrix_[14];
            }
        }
        actors_.push_back(compActor);
        allyCount_++;
    }

    // 3. Process Local Player
    if (worldChrMan != 0 && localChr != 0) {
        TrackedActor localActor{};
        localActor.chrIns = localChr;

        SafeRead(localChr + 0x48u, localActor.chrModel);
        if (localActor.chrModel != 0) {
            fastLocalModel_.store(localActor.chrModel, std::memory_order_release);
            SafeRead(localActor.chrModel + 0x8u, localActor.drawEntity);
            if (localActor.drawEntity != 0) {
                SafeRead(localActor.drawEntity + 0x20u, localActor.meshCount);
                SafeRead(localActor.drawEntity + 0xe8u, localActor.flverModel);
                if (localActor.flverModel != 0) {
                    SafeRead(localActor.flverModel + 0x8u, localActor.flverData);
                }
                if (!localActor.flverData) {
                    SafeRead(localActor.drawEntity + 0xb00u, localActor.flverData);
                }
                fastLocalEntity_.store(localActor.drawEntity, std::memory_order_release);
            } else {
                fastLocalEntity_.store(0, std::memory_order_release);
            }

            std::uintptr_t p20b0 = 0;
            if (SafeRead(localChr + 0x20b0u, p20b0) && p20b0 != 0) {
                SafeRead(p20b0 + 0x8u, localActor.asmEntity);
            }
            if (!IsValidUserPointer(reinterpret_cast<void*>(localActor.asmEntity), sizeof(std::uintptr_t))) {
                localActor.asmEntity = 0;
            }
            fastLocalAsmEntity_.store(localActor.asmEntity, std::memory_order_release);
        } else {
            fastLocalModel_.store(0, std::memory_order_release);
            fastLocalEntity_.store(0, std::memory_order_release);
            fastLocalAsmEntity_.store(0, std::memory_order_release);
        }

        localActor.isLocal = true;
        localActor.isAlly = false;
        localActor.lastSeenFrame = frameNumber;
        std::snprintf(localActor.nativeType, sizeof(localActor.nativeType), "LocalPlayer");
        SafeRead(localChr + 0x70u, localActor.charType);
        SafeRead(localChr + 0x74u, localActor.teamType);

        bool hasLocalPos = TryReadActorPosition(localChr, localActor.position);
        if (!hasLocalPos && localActor.chrModel) {
            hasLocalPos = TryReadActorPositionFromModel(localActor.chrModel, localActor.position);
        }
        if (hasLocalPos) {
            if (hasValidCamera_) {
                localActor.relPosition[0] = localActor.position[0] - cameraPos_[0];
                localActor.relPosition[1] = localActor.position[1] - cameraPos_[1];
                localActor.relPosition[2] = localActor.position[2] - cameraPos_[2];

                localActor.viewPosition[0] = localActor.position[0] * viewMatrix_[0] + localActor.position[1] * viewMatrix_[4] + localActor.position[2] * viewMatrix_[8] + viewMatrix_[12];
                localActor.viewPosition[1] = localActor.position[0] * viewMatrix_[1] + localActor.position[1] * viewMatrix_[5] + localActor.position[2] * viewMatrix_[9] + viewMatrix_[13];
                localActor.viewPosition[2] = localActor.position[0] * viewMatrix_[2] + localActor.position[1] * viewMatrix_[6] + localActor.position[2] * viewMatrix_[10] + viewMatrix_[14];
            }
        }
        actors_.push_back(localActor);
    }

        // Remote player slots in world + 0x40 (count at +0x38)
        std::uint32_t slotCount = 0;
        std::uintptr_t slotsArray = 0;
        if (SafeRead(worldChrMan + 0x38u, slotCount) &&
            SafeRead(worldChrMan + 0x40u, slotsArray) &&
            slotCount > 0 && slotCount <= 32 && slotsArray != 0) {

            for (std::uint32_t i = 0; i < slotCount; ++i) {
                std::uintptr_t remoteChr = 0;
                // Each slot has a step of 0x38
                if (!SafeRead(slotsArray + i * 0x38u, remoteChr) || !remoteChr) continue;
                if (remoteChr == localChr) continue;

                // Avoid duplicates with companion
                bool alreadyTracked = false;
                for (const auto& a : actors_) {
                    if (a.chrIns == remoteChr) { alreadyTracked = true; break; }
                }
                if (alreadyTracked) continue;

                TrackedActor remoteActor{};
                remoteActor.chrIns = remoteChr;
                SafeRead(remoteChr + 0x48u, remoteActor.chrModel);
                if (remoteActor.chrModel != 0) {
                    SafeRead(remoteActor.chrModel + 0x8u, remoteActor.drawEntity);
                    if (remoteActor.drawEntity != 0) {
                        SafeRead(remoteActor.drawEntity + 0x20u, remoteActor.meshCount);
                        SafeRead(remoteActor.drawEntity + 0xe8u, remoteActor.flverModel);
                        if (remoteActor.flverModel != 0) {
                            SafeRead(remoteActor.flverModel + 0x8u, remoteActor.flverData);
                        }
                        if (!remoteActor.flverData) {
                            SafeRead(remoteActor.drawEntity + 0xb00u, remoteActor.flverData);
                        }
                    }
                    std::uintptr_t p20b0 = 0;
                    if (SafeRead(remoteChr + 0x20b0u, p20b0) && p20b0 != 0) {
                        SafeRead(p20b0 + 0x8u, remoteActor.asmEntity);
                    }
                    if (!IsValidUserPointer(reinterpret_cast<void*>(remoteActor.asmEntity), sizeof(std::uintptr_t))) {
                        remoteActor.asmEntity = 0;
                    }
                }
                remoteActor.isLocal = false;
                remoteActor.isAlly = true; // Player in coop slot
                remoteActor.lastSeenFrame = frameNumber;
                std::snprintf(remoteActor.nativeType, sizeof(remoteActor.nativeType), "RemotePlayer[%u]", i);
                SafeRead(remoteChr + 0x70u, remoteActor.charType);
                SafeRead(remoteChr + 0x74u, remoteActor.teamType);

                bool hasRemotePos = TryReadActorPosition(remoteChr, remoteActor.position);
                if (!hasRemotePos && remoteActor.chrModel) {
                    hasRemotePos = TryReadActorPositionFromModel(remoteActor.chrModel, remoteActor.position);
                }
                if (hasRemotePos) {
                    if (hasValidCamera_) {
                        remoteActor.relPosition[0] = remoteActor.position[0] - cameraPos_[0];
                        remoteActor.relPosition[1] = remoteActor.position[1] - cameraPos_[1];
                        remoteActor.relPosition[2] = remoteActor.position[2] - cameraPos_[2];

                        remoteActor.viewPosition[0] = remoteActor.position[0] * viewMatrix_[0] + remoteActor.position[1] * viewMatrix_[4] + remoteActor.position[2] * viewMatrix_[8] + viewMatrix_[12];
                        remoteActor.viewPosition[1] = remoteActor.position[0] * viewMatrix_[1] + remoteActor.position[1] * viewMatrix_[5] + remoteActor.position[2] * viewMatrix_[9] + viewMatrix_[13];
                        remoteActor.viewPosition[2] = remoteActor.position[0] * viewMatrix_[2] + remoteActor.position[1] * viewMatrix_[6] + remoteActor.position[2] * viewMatrix_[10] + viewMatrix_[14];
                    }
                }
                actors_.push_back(remoteActor);
                allyCount_++;
            }
        }

    const bool includeLocal = (ds3scPlayerOutlineEnable != 0);
    std::array<std::uintptr_t, kMaxFastAllies> fastList{};
    std::size_t fastTotal = 0;
    auto addFast = [&](std::uintptr_t e) {
        if (!e) return;
        for (std::size_t i = 0; i < fastTotal; ++i) {
            if (fastList[i] == e) return;
        }
        if (fastTotal < kMaxFastAllies) fastList[fastTotal++] = e;
    };
    const bool inCutscene = IsInCutscene();
    for (const auto& a : actors_) {
        if (a.isAlly) {
            if (inCutscene) {
                // Suppress ally rendering during cutscenes so allies do not intrude
                // on the cinematic camera or break ending/boss cutscenes.
                if (a.drawEntity) HideAllyFromCutscene(a.drawEntity);
                if (a.asmEntity) HideAllyFromCutscene(a.asmEntity);
            } else {
                if (a.drawEntity) {
                    addFast(a.drawEntity);
                    MakeAllyPersistentlyVisible(a.drawEntity);
                }
                if (a.asmEntity) {
                    addFast(a.asmEntity);
                    MakeAllyPersistentlyVisible(a.asmEntity);
                }
            }
        } else if (a.isLocal && includeLocal) {
            if (!inCutscene) {
                if (a.drawEntity) MakeAllyPersistentlyVisible(a.drawEntity);
                if (a.asmEntity) MakeAllyPersistentlyVisible(a.asmEntity);
            }
        }
    }
    for (std::size_t i = 0; i < fastTotal; ++i) {
        fastAllyEntities_[i].store(fastList[i], std::memory_order_relaxed);
    }
    for (std::size_t i = fastTotal; i < kMaxFastAllies; ++i) {
        fastAllyEntities_[i].store(0, std::memory_order_relaxed);
    }
    fastAllyCount_.store(fastTotal, std::memory_order_release);

    std::array<FastAllyBounds, kMaxFastAllies> fastBounds{};
    std::size_t boundsCount = 0;
    for (const auto& a : actors_) {
        if ((a.isAlly || (a.isLocal && includeLocal)) && boundsCount < kMaxFastAllies) {
            float minPos[3] = { -999999.0f, -999999.0f, -999999.0f };
            float maxPos[3] = {  999999.0f,  999999.0f,  999999.0f };
            bool hasBounds = false;

            if (std::isfinite(a.position[0]) && std::isfinite(a.position[1]) && std::isfinite(a.position[2]) &&
                (std::abs(a.position[0]) > 0.01f || std::abs(a.position[1]) > 0.01f || std::abs(a.position[2]) > 0.01f)) {
                minPos[0] = a.position[0] - 5.0f;
                minPos[1] = a.position[1] - 4.0f;
                minPos[2] = a.position[2] - 5.0f;
                maxPos[0] = a.position[0] + 5.0f;
                maxPos[1] = a.position[1] + 6.0f;
                maxPos[2] = a.position[2] + 5.0f;
                hasBounds = true;
            }

            if (hasBounds) {
                fastBounds[boundsCount].minX = minPos[0];
                fastBounds[boundsCount].minY = minPos[1];
                fastBounds[boundsCount].minZ = minPos[2];
                fastBounds[boundsCount].maxX = maxPos[0];
                fastBounds[boundsCount].maxY = maxPos[1];
                fastBounds[boundsCount].maxZ = maxPos[2];
                ++boundsCount;
            }
        }
    }
    fastAllyBounds_ = fastBounds;
    fastAllyBoundsCount_.store(boundsCount, std::memory_order_release);

    std::array<std::uintptr_t, kMaxFastAllies * 4> fastModels{};
    std::size_t fastModelTotal = 0;
    auto addFastModel = [&](std::uintptr_t m) {
        if (!m) return;
        for (std::size_t i = 0; i < fastModelTotal; ++i) {
            if (fastModels[i] == m) return;
        }
        if (fastModelTotal < kMaxFastAllies * 4) fastModels[fastModelTotal++] = m;
    };
    for (const auto& a : actors_) {
        if (a.isAlly && a.chrModel) {
            addFastModel(a.chrModel);
        }
    }
    for (std::size_t i = 0; i < fastModelTotal; ++i) {
        fastAllyModels_[i].store(fastModels[i], std::memory_order_relaxed);
    }
    fastAllyModelCount_.store(fastModelTotal, std::memory_order_release);

    // Rate-limiting informational logs
    const auto now = GetTickCount64();
    if (now - lastLogTime_ > 2000) {
        lastLogTime_ = now;
        if (!actors_.empty()) {
            char logBuffer[256];
            std::snprintf(logBuffer, sizeof(logBuffer),
                "[allyMask][TRACKER] Tracked actors: %zu (Allies: %zu, CamValid=%d CamPos=(%.1f, %.1f, %.1f))\n",
                actors_.size(), allyCount_.load(), hasValidCamera_ ? 1 : 0,
                cameraPos_[0], cameraPos_[1], cameraPos_[2]);
            OutputDebugStringA(logBuffer);
            for (const auto& a : actors_) {
                std::snprintf(logBuffer, sizeof(logBuffer),
                    "    -> Actor=0x%p Model=0x%p Draw=0x%p Pos=(%.1f, %.1f, %.1f) Rel=(%.1f, %.1f, %.1f) isAlly=%d Type=%s\n",
                    reinterpret_cast<void*>(a.chrIns),
                    reinterpret_cast<void*>(a.chrModel),
                    reinterpret_cast<void*>(a.drawEntity),
                    a.position[0], a.position[1], a.position[2],
                    a.relPosition[0], a.relPosition[1], a.relPosition[2],
                    a.isAlly ? 1 : 0, a.nativeType);
                OutputDebugStringA(logBuffer);
            }
        }
    }
}

bool ActorTracker::FastCheckAllyMatch(const float* data, std::size_t byteWidth) const noexcept {
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    if (!data || byteWidth < 48 || allyCount_ == 0) return false;

    const std::size_t floatCount = std::min<std::size_t>(byteWidth / sizeof(float), 64u);

    // Find local player to safely exclude them
    const TrackedActor* local = nullptr;
    for (const auto& a : actors_) {
        if (a.isLocal) { local = &a; break; }
    }

    auto distSq3 = [](float x1, float y1, float z1, float x2, float y2, float z2) noexcept -> float {
        const float dx = x1 - x2;
        const float dy = y1 - y2;
        const float dz = z1 - z2;
        return dx * dx + dy * dy + dz * dz;
    };

    auto checkVector = [&](float x, float y, float z) noexcept -> bool {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;

        // Discard null or degenerate vectors
        if (std::abs(x) < 0.001f && std::abs(y) < 0.001f && std::abs(z) < 0.001f) return false;

        // If matching local player (< 1.5m), it is not the ally
        if (local) {
            if (distSq3(x, y, z, local->position[0], local->position[1], local->position[2]) < 2.25f) return false;
            if (hasValidCamera_ && distSq3(x, y, z, local->relPosition[0], local->relPosition[1], local->relPosition[2]) < 2.25f) return false;
        }

        // Check against allies
        for (const auto& a : actors_) {
            if (!a.isAlly) continue;

            // 1. Absolute world coordinates (distance <= 2.0 meters)
            if (distSq3(x, y, z, a.position[0], a.position[1], a.position[2]) < 4.0f) {
                return true;
            }
            // 2. Coordinates relative to camera
            if (hasValidCamera_ && distSq3(x, y, z, a.relPosition[0], a.relPosition[1], a.relPosition[2]) < 4.0f) {
                return true;
            }
            // 3. View space coordinates (view matrix)
            if (hasValidCamera_ && distSq3(x, y, z, a.viewPosition[0], a.viewPosition[1], a.viewPosition[2]) < 4.0f) {
                return true;
            }
        }
        return false;
    };

    // 1. Test typical transform matrix positions:
    // Column-major 4x4 format (translation in column 3: floats 12, 13, 14)
    if (floatCount >= 16) {
        if (checkVector(data[12], data[13], data[14])) return true;
    }
    // Row-major 3x4 or 4x4 format (translation in floats 3, 7, 11)
    if (floatCount >= 12) {
        if (checkVector(data[3], data[7], data[11])) return true;
    }
    // Second consecutive matrix (offset 64 bytes = float 16)
    if (floatCount >= 32) {
        if (checkVector(data[28], data[29], data[30])) return true;
        if (checkVector(data[19], data[23], data[27])) return true;
    }

    // 2. Test as float3 vector at any 16-byte aligned boundary (float4)
    for (std::size_t i = 0; i + 3 <= floatCount; i += 4) {
        if (checkVector(data[i], data[i + 1], data[i + 2])) {
            return true;
        }
    }

    return false;
}

CorrelationResult ActorTracker::CheckConstantBufferCorrelation(
    std::uint32_t slot,
    const void* cbData,
    std::size_t cbSize
) noexcept {
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    CorrelationResult result{};
    // Scene-global constants and nearest-position matches cannot identify an
    // ally. Compare every element of the actor's actual world transform and
    // reject ambiguous matches, including local-player/ally overlap.
    if (slot == 0 || !cbData) return result;
    const TrackedActor* matched = nullptr;
    for (const auto& actor : actors_) {
        if (!actor.chrModel) continue;
        std::array<float, 12> world{};
        if (!SafeReadBytes(actor.chrModel + 0x20, world.data(), sizeof(world)) ||
            !MatchesWorldTransform(world, cbData, cbSize)) continue;
        if (matched && matched->chrIns != actor.chrIns) return {};
        matched = &actor;
    }
    if (matched) {
        result.matchedActor = matched->chrIns;
        result.matchedCbSlot = slot;
        result.deltaDistance = 0;
        result.isAllyDraw = matched->isAlly;
        result.isLocalDraw = matched->isLocal;
    }
    return result;
}

bool ActorTracker::BoundsContainAlly(const float* b) const noexcept {
    if (!b) return false;
    const auto count = fastAllyBoundsCount_.load(std::memory_order_acquire);
    if (count == 0) return false;

    // b[0..2] is min, b[4..6] is max
    const float bMinX = b[0], bMinY = b[1], bMinZ = b[2];
    const float bMaxX = b[4], bMaxY = b[5], bMaxZ = b[6];

    if (!std::isfinite(bMinX) || !std::isfinite(bMaxX) ||
        !std::isfinite(bMinY) || !std::isfinite(bMaxY) ||
        !std::isfinite(bMinZ) || !std::isfinite(bMaxZ)) return false;
    if (bMinX > bMaxX || bMinY > bMaxY || bMinZ > bMaxZ) return false;

    for (std::size_t i = 0; i < count; ++i) {
        const auto& ab = fastAllyBounds_[i];
        if (bMinX <= ab.maxX && bMaxX >= ab.minX &&
            bMinY <= ab.maxY && bMaxY >= ab.minY &&
            bMinZ <= ab.maxZ && bMaxZ >= ab.minZ) {
            return true;
        }
    }
    return false;
}

ActorTracker::CameraData ActorTracker::GetCameraData() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    return cameraData_;
}

bool ActorTracker::ProjectWorldToScreen(
    const float worldPos[3],
    float& outScreenX,
    float& outScreenY,
    float& outDistance,
    float screenW,
    float screenH,
    float heightOffset
) const noexcept {
    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    if (!cameraData_.valid || screenW <= 0.0f || screenH <= 0.0f) return false;

    // Overhead indicator sits above character head (~1.35m above root feet pos)
    const float dx = worldPos[0] - cameraData_.pos[0];
    const float dy = (worldPos[1] + heightOffset) - cameraData_.pos[1];
    const float dz = worldPos[2] - cameraData_.pos[2];

    const float xCam = dx * cameraData_.right[0] + dy * cameraData_.right[1] + dz * cameraData_.right[2];
    const float yCam = dx * cameraData_.up[0] + dy * cameraData_.up[1] + dz * cameraData_.up[2];
    const float zCam = dx * cameraData_.forward[0] + dy * cameraData_.forward[1] + dz * cameraData_.forward[2];

    outDistance = zCam;
    if (zCam <= 0.2f) return false; // Behind or clipping through camera lens

    const float tanHalf = std::tan(cameraData_.fovY * 0.5f);
    if (tanHalf <= 0.0001f || cameraData_.aspect <= 0.0001f) return false;
    const float fy = 1.0f / tanHalf;
    const float fx = fy / cameraData_.aspect;

    const float xNdc = (xCam * fx) / zCam;
    const float yNdc = (yCam * fy) / zCam;

    outScreenX = (xNdc + 1.0f) * 0.5f * screenW;
    outScreenY = (1.0f - yNdc) * 0.5f * screenH;
    return true;
}

bool ActorTracker::IsInCutscene() const noexcept {
    auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"DarkSoulsIII.exe"));
    if (!gameBase) gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return false;

    std::uintptr_t newMenuSystem = 0;
    if (SafeRead(gameBase + kCandidateNewMenuSystemRva, newMenuSystem) && newMenuSystem != 0) {
        // Offset 0x3084 in NewMenuSystem: 1 = cutscene active, 0 = normal gameplay
        std::uint8_t inCutscene = 0;
        if (SafeRead(newMenuSystem + 0x3084u, inCutscene) && inCutscene != 0) {
            return true;
        }
    }
    return false;
}

bool ActorTracker::IsGameMenuOpen() const noexcept {
#if !defined(DS3SC_STANDALONE_TEST)
    // Check mod's own config/settings modal overlay (F11/F7)
    if (TitleMenu::Instance().IsModalOpen()) return true;
#endif

    // Do not draw screen-space markers while the game is still loading or a
    // native DS3 menu owns the scene.  This function is read-only and uses the
    // guarded reads already used by the actor tracker.
    {
        std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
        if (!cameraData_.valid) return true;
    }

    auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"DarkSoulsIII.exe"));
    if (!gameBase) gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return true;

    std::uintptr_t worldChrMan = 0;
    std::uintptr_t localChr = 0;
    if (!SafeRead(gameBase + kCandidateWorldChrManRva, worldChrMan) || !worldChrMan ||
        !SafeRead(worldChrMan + 0x80u, localChr) || !localChr) {
        return true;
    }

    // NewMenuSystem + 0x3084 is a debug help-menu option, not a cutscene
    // flag. Marker visibility follows the actual frontend state and the local
    // event animation without changing actor visibility or any saved toggle.
    return NativeSceneSuppressesMarkers(gameBase, localChr,
        [](std::uintptr_t address, auto& value) noexcept { return SafeRead(address, value); });
}

bool ActorTracker::IsActorOccluded(const TrackedActor& a) const noexcept {
    std::uintptr_t drawEntity = a.drawEntity;
    if (!drawEntity && a.chrModel) {
        SafeRead(a.chrModel + 0x8u, drawEntity);
    }
    if (drawEntity) {
        std::uint8_t occluded = 0;
        // DrawEntity + 0xa5d is DS3's native wall occlusion flag (0 = visible, != 0 = occluded behind wall)
        if (SafeRead(drawEntity + 0xa5du, occluded) && occluded != 0) {
            return true;
        }
    }
    return false;
}

bool ActorTracker::IsRayOccludedByLocalPlayer(const float targetWorldPos[3]) const noexcept {
    if (!targetWorldPos) return false;

    // Find local player actor
    const TrackedActor* localActor = nullptr;
    for (const auto& a : actors_) {
        if (a.isLocal) {
            localActor = &a;
            break;
        }
    }
    if (!localActor) return false;

    const float* cam = cameraData_.pos;
    const float* target = targetWorldPos;
    const float* local = localActor->position;

    // A marker above the head is intentionally allowed to remain visible. The
    // previous vertical capsule made a nearly-overhead marker vanish as soon
    // as its projected ray grazed the character's neck.
    if (target[1] > local[1] + 1.75f) return false;

    // Vector from camera to target marker
    const float d1x = target[0] - cam[0];
    const float d1y = target[1] - cam[1];
    const float d1z = target[2] - cam[2];
    const float rayLenSq = d1x * d1x + d1y * d1y + d1z * d1z;
    if (rayLenSq <= 0.0001f) return false;

    // Vector from camera to local player character
    const float toLocalX = local[0] - cam[0];
    const float toLocalY = (local[1] + 0.95f) - cam[1];
    const float toLocalZ = local[2] - cam[2];
    const float localDistSq = toLocalX * toLocalX + toLocalY * toLocalY + toLocalZ * toLocalZ;

    // If marker is closer to camera than local player, local player cannot occlude it
    if (rayLenSq < localDistSq) return false;

    // Local player body only: keep the occlusion volume narrower than the
    // native collision capsule so a nearby ally marker is not suppressed just
    // because the two characters are standing shoulder-to-shoulder.
    const float p3x = local[0];
    const float p3y = local[1] + 0.25f;
    const float p3z = local[2];

    const float d2x = 0.0f;
    const float d2y = 1.20f; // Torso only; head/overhead markers remain visible
    const float d2z = 0.0f;

    // Segment 1: Cam (p1) to Target (p2 = p1 + d1)
    // Segment 2: P3 to P4 (p3 + d2)
    // Find closest distance between segments
    const float rx = cam[0] - p3x;
    const float ry = cam[1] - p3y;
    const float rz = cam[2] - p3z;

    const float a = rayLenSq; // dot(d1, d1)
    const float e = d2y * d2y; // dot(d2, d2)
    const float f = d2y * ry;  // dot(d2, r)
    const float c = d1x * rx + d1y * ry + d1z * rz; // dot(d1, r)
    const float b = d1y * d2y; // dot(d1, d2)

    const float denom = a * e - b * b;
    float s = 0.0f;
    if (std::abs(denom) > 1e-6f) {
        s = (b * f - c * e) / denom;
        if (s < 0.0f) s = 0.0f;
        else if (s > 1.0f) s = 1.0f;
    }

    float t = (b * s + f) / e;
    if (t < 0.0f) {
        t = 0.0f;
        s = -c / a;
        if (s < 0.0f) s = 0.0f;
        else if (s > 1.0f) s = 1.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
        s = (b - c) / a;
        if (s < 0.0f) s = 0.0f;
        else if (s > 1.0f) s = 1.0f;
    }

    // Closest point on ray: cam + d1 * s
    // Closest point on capsule segment: p3 + d2 * t
    const float c1x = cam[0] + d1x * s;
    const float c1y = cam[1] + d1y * s;
    const float c1z = cam[2] + d1z * s;

    const float c2x = p3x + d2x * t;
    const float c2y = p3y + d2y * t;
    const float c2z = p3z + d2z * t;

    const float distX = c1x - c2x;
    const float distY = c1y - c2y;
    const float distZ = c1z - c2z;
    const float distSq = distX * distX + distY * distY + distZ * distZ;

    // A tight visual body radius.  The previous 0.50m radius made markers
    // disappear while an ally was merely close to the player's shoulder.
    constexpr float kCharRadius = 0.28f;
    constexpr float kCharRadiusSq = kCharRadius * kCharRadius;

    // Occluded if closest distance is within capsule radius AND
    // the intersection occurs between camera and marker (s is in (0.02, 0.95))
    return (distSq <= kCharRadiusSq && s > 0.02f && s < 0.95f);
}

extern "C" {
__declspec(dllexport) volatile LONG ds3scDiamondMarkerHeightCm = 135;
}

void ActorTracker::SetMarkerHeightOffset(float offset) noexcept {
    markerHeightOffset_.store(offset, std::memory_order_relaxed);
    if (offset >= 0.5f && offset <= 5.0f) {
        InterlockedExchange(&ds3scDiamondMarkerHeightCm, static_cast<LONG>(std::round(offset * 100.0f)));
    }
}

float ActorTracker::GetMarkerHeightOffset() const noexcept {
    const LONG cm = ds3scDiamondMarkerHeightCm;
    if (cm > 0) return cm / 100.0f;
    return markerHeightOffset_.load(std::memory_order_relaxed);
}

void ActorTracker::LoadSettingsFromIni() noexcept {
    settingsLoaded_ = true;
    char iniPath[MAX_PATH] = {};
    HMODULE hModule = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ActorTracker::Instance), &hModule) && hModule) {
        GetModuleFileNameA(hModule, iniPath, sizeof(iniPath));
        char* lastSlash = strrchr(iniPath, '\\');
        if (lastSlash) {
            strcpy_s(lastSlash + 1, sizeof(iniPath) - (lastSlash + 1 - iniPath), "ds3sc_settings.ini");
        }
    }
    if (iniPath[0] == '\0' || GetFileAttributesA(iniPath) == INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesA("TheAshenLink\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath, "TheAshenLink\\ds3sc_settings.ini");
        } else if (GetFileAttributesA("SeamplusCoop\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath, "SeamplusCoop\\ds3sc_settings.ini");
        } else if (GetFileAttributesA("SeamlessCoop\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath, "SeamlessCoop\\ds3sc_settings.ini");
        }
    }

    if (iniPath[0] != '\0' && GetFileAttributesA(iniPath) != INVALID_FILE_ATTRIBUTES) {
        char heightBuf[64] = {};
        if (GetPrivateProfileStringA("ALLY_MARKERS", "height_offset", "", heightBuf, sizeof(heightBuf), iniPath) > 0) {
            char* end = nullptr;
            float val = std::strtof(heightBuf, &end);
            if (end != heightBuf && val >= 0.5f && val <= 5.0f) {
                markerHeightOffset_.store(val, std::memory_order_relaxed);
                InterlockedExchange(&ds3scDiamondMarkerHeightCm, static_cast<LONG>(std::round(val * 100.0f)));
            }
        }
    }
}

std::size_t ActorTracker::GetAllyProjections(
    AllyScreenProjection* outProjections,
    std::size_t maxCount,
    float screenW,
    float screenH,
    bool cullLocalPlayer
) const noexcept {
    if (!outProjections || maxCount == 0) return 0;

    std::lock_guard<std::recursive_mutex> lock(actorsMutex_);
    if (!cameraData_.valid || screenW <= 0.0f || screenH <= 0.0f) return 0;

    const float markerHeight = GetMarkerHeightOffset();
    std::size_t count = 0;
    for (const auto& a : actors_) {
        if (!a.isAlly || a.isLocal || count >= maxCount) continue;

        const float markerWorldPos[3] = {
            a.position[0], a.position[1] + markerHeight, a.position[2]
        };

        // When the renderer has a current-frame local-player mask, the pixel
        // shader performs exact silhouette occlusion. Do not also apply the
        // coarse CPU capsule in that mode: it can reject an overhead marker
        // before the silhouette mask gets a chance to decide per-pixel.
        if (cullLocalPlayer && IsRayOccludedByLocalPlayer(markerWorldPos)) continue;

        float sx = 0.0f, sy = 0.0f, dist = 0.0f;
        // Project diamond marker overhead
        if (ProjectWorldToScreen(a.position, sx, sy, dist, screenW, screenH, markerHeight)) {
            // Cull off-screen projections
            if (sx < -20.0f || sx > screenW + 20.0f || sy < -20.0f || sy > screenH + 20.0f) {
                continue;
            }

            outProjections[count].screenX = sx;
            outProjections[count].screenY = sy;
            outProjections[count].distance = dist;
            outProjections[count].inFront = true;
            ++count;
        }
    }
    return count;
}

} // namespace ds3sc::render
