#include "cutscene_fix_extension.h"
#include "../../render/ally_pass_visibility.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
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

void SetEntityCpuVisibility(std::uintptr_t entity, bool visible) noexcept {
    if (!entity || !IsValidUserPointer(reinterpret_cast<void*>(entity), 0xc40)) return;
    __try {
        volatile auto* pVis1 = reinterpret_cast<volatile std::uint8_t*>(entity + 0xc3c);
        volatile auto* pVis2 = reinterpret_cast<volatile std::uint8_t*>(entity + 0xbb0);
        if (visible) {
            *pVis1 |= 1;
            *pVis2 |= 1;
        } else {
            *pVis1 &= static_cast<std::uint8_t>(~1);
            *pVis2 &= static_cast<std::uint8_t>(~1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

CutsceneFixExtension* g_pCutsceneFixInstance = nullptr;

} // namespace

CutsceneFixExtension::CutsceneFixExtension() noexcept {
    g_pCutsceneFixInstance = this;
}

bool CutsceneFixExtension::Initialize() noexcept {
    initialized_.store(true, std::memory_order_release);
    OutputDebugStringA("[ds3sc-cutscene-fix] Initialized native cutscene ally isolation module.\n");
    return true;
}

void CutsceneFixExtension::Shutdown() noexcept {
    initialized_.store(false, std::memory_order_release);

    // If was in cutscene during shutdown, ensure allies are made visible again
    if (wasInCutscene_.exchange(false, std::memory_order_acq_rel)) {
        const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (gameBase) {
            std::uintptr_t worldChrMan = 0;
            if (SafeRead(gameBase + kCandidateWorldChrManRva, worldChrMan) && worldChrMan) {
                std::uint32_t slotCount = 0;
                std::uintptr_t slotsArray = 0;
                if (SafeRead(worldChrMan + 0x38u, slotCount) && SafeRead(worldChrMan + 0x40u, slotsArray) &&
                    slotCount <= 32 && slotsArray) {
                    for (std::uint32_t i = 0; i < slotCount; ++i) {
                        std::uintptr_t chr = 0;
                        if (SafeRead(slotsArray + i * 0x38u, chr) && chr) {
                            std::uintptr_t model = 0;
                            if (SafeRead(chr + 0x48u, model) && model) {
                                std::uintptr_t draw = 0;
                                if (SafeRead(model + 0x8u, draw) && draw) {
                                    SetEntityCpuVisibility(draw, true);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    OutputDebugStringA("[ds3sc-cutscene-fix] Shutdown cutscene ally isolation module.\n");
}

bool CutsceneFixExtension::IsInCutscene() const noexcept {
    const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return false;

    std::uintptr_t menuSystem = 0;
    if (!SafeRead(gameBase + kCandidateNewMenuSystemRva, menuSystem) || !menuSystem) {
        return false;
    }

    std::uint8_t cutsceneFlag = 0;
    if (SafeRead(menuSystem + 0x3084u, cutsceneFlag)) {
        return (cutsceneFlag != 0);
    }
    return false;
}

std::uint32_t CutsceneFixExtension::GetCutscenesHandled() const noexcept {
    return cutscenesHandled_.load(std::memory_order_relaxed);
}

void CutsceneFixExtension::OnTick() noexcept {
    if (!initialized_.load(std::memory_order_relaxed)) {
        return;
    }
    ProcessCutsceneState();
}

void CutsceneFixExtension::ProcessCutsceneState() noexcept {
    const bool inCutscene = IsInCutscene();
    const bool wasIn = wasInCutscene_.load(std::memory_order_acquire);

    const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!gameBase) return;

    std::uintptr_t worldChrMan = 0;
    SafeRead(gameBase + kCandidateWorldChrManRva, worldChrMan);

    auto forEachAlly = [&](auto&& callback) {
        if (worldChrMan) {
            std::uint32_t slotCount = 0;
            std::uintptr_t slotsArray = 0;
            if (SafeRead(worldChrMan + 0x38u, slotCount) && SafeRead(worldChrMan + 0x40u, slotsArray) &&
                slotCount <= 32 && slotsArray) {
                for (std::uint32_t i = 0; i < slotCount; ++i) {
                    std::uintptr_t chr = 0;
                    if (SafeRead(slotsArray + i * 0x38u, chr) && chr) {
                        callback(chr);
                    }
                }
            }
        }
        CompanionExportStatus comp{};
        if (ReadCompanionStatus(comp) && comp.abi == 1 && comp.state == 3 && comp.actor != 0) {
            callback(comp.actor);
        }
    };

    if (inCutscene) {
        if (!wasIn) {
            wasInCutscene_.store(true, std::memory_order_release);
            cutscenesHandled_.fetch_add(1, std::memory_order_relaxed);
            OutputDebugStringA("[ds3sc-cutscene-fix] Cutscene entered; isolating co-op allies.\n");
        }

        // Keep allies hidden while cutscene is active
        forEachAlly([](std::uintptr_t chr) {
            std::uintptr_t model = 0;
            if (SafeRead(chr + 0x48u, model) && model) {
                std::uintptr_t draw = 0;
                if (SafeRead(model + 0x8u, draw) && draw) {
                    SetEntityCpuVisibility(draw, false);
                }
            }
            std::uintptr_t p20b0 = 0;
            if (SafeRead(chr + 0x20b0u, p20b0) && p20b0) {
                std::uintptr_t asmDraw = 0;
                if (SafeRead(p20b0 + 0x8u, asmDraw) && asmDraw) {
                    SetEntityCpuVisibility(asmDraw, false);
                }
            }
        });
    } else {
        if (wasIn) {
            wasInCutscene_.store(false, std::memory_order_release);
            OutputDebugStringA("[ds3sc-cutscene-fix] Cutscene ended; restoring ally visibility.\n");

            // Restore visibility immediately
            forEachAlly([](std::uintptr_t chr) {
                std::uintptr_t model = 0;
                if (SafeRead(chr + 0x48u, model) && model) {
                    std::uintptr_t draw = 0;
                    if (SafeRead(model + 0x8u, draw) && draw) {
                        SetEntityCpuVisibility(draw, true);
                    }
                }
                std::uintptr_t p20b0 = 0;
                if (SafeRead(chr + 0x20b0u, p20b0) && p20b0) {
                    std::uintptr_t asmDraw = 0;
                    if (SafeRead(p20b0 + 0x8u, asmDraw) && asmDraw) {
                        SetEntityCpuVisibility(asmDraw, true);
                    }
                }
            });
        }
    }
}

std::shared_ptr<CutsceneFixExtension> CreateCutsceneFixExtension() noexcept {
    return std::make_shared<CutsceneFixExtension>();
}

} // namespace ds3sc::extensions

extern "C" {

bool ds3sc_is_in_cutscene() {
    if (ds3sc::extensions::g_pCutsceneFixInstance) {
        return ds3sc::extensions::g_pCutsceneFixInstance->IsInCutscene();
    }
    return false;
}

uint32_t ds3sc_get_cutscenes_handled() {
    if (ds3sc::extensions::g_pCutsceneFixInstance) {
        return ds3sc::extensions::g_pCutsceneFixInstance->GetCutscenesHandled();
    }
    return 0;
}

}
