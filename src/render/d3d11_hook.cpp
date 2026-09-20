#include "d3d11_hook.h"
#if DS3SC_FEATURE_OUTLINE_CAPTURE
#include "ally_outline.h"
#include "native_draw_packet.h"
#include "ally_pass_visibility.h"
#endif
#if DS3SC_FEATURE_OUTLINE_CAPTURE || (defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS)
#include "actor_tracker.h"
#endif
#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
#include "ally_marker.h"
#endif
#if (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS) || (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS)
#include "stats_overlay.h"
#endif
#include "title_menu.h"
#include "../../tools/vendor/minhook-1.3.4/include/MinHook.h"
#include <array>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace ds3sc::render {
namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using IndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using InstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using IndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using ClearFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using FinishFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
PresentFn originalPresent{};
ResizeFn originalResize{};
DrawFn originalDraw[2]{};
IndexedFn originalIndexed[2]{};
InstancedFn originalInstanced[2]{};
IndexedInstancedFn originalIndexedInstanced[2]{};
ClearFn originalClear[2]{};
FinishFn originalFinish{};
ExecuteFn originalExecute{};
std::mutex installMutex;
std::recursive_mutex allyModelMutex;
std::vector<void*> ownedHooks;
struct PresentationHook {
    void* target = nullptr;
    std::array<unsigned char, 5> originalBytes{};
};
std::array<PresentationHook, 2> presentationHooks{};
bool ReadPresentationEntry(void* target, std::array<unsigned char, 5>& bytes) noexcept {
    if (!target) return false;
    __try { std::memcpy(bytes.data(), target, bytes.size()); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* ResolvePresentationEntry(void* entry) noexcept {
    // Steam can temporarily restore/reapply the DXGI entry on every Present.
    // Hook the existing forwarding destination, so that operation cannot
    // erase our detour. Never change a shared COM vtable or bypass the overlay.
    std::array<void*, 8> visited{};
    __try {
        for (std::size_t depth = 0; depth < visited.size(); ++depth) {
            if (!entry) return nullptr;
            for (std::size_t i = 0; i < depth; ++i) if (visited[i] == entry) return nullptr;
            visited[depth] = entry;
            const auto* bytes = static_cast<const unsigned char*>(entry);
            std::int32_t displacement = 0;
            if (bytes[0] == 0xe9) {
                std::memcpy(&displacement, bytes + 1, sizeof(displacement));
                entry = const_cast<unsigned char*>(bytes + 5 + displacement);
            } else if (bytes[0] == 0xff && bytes[1] == 0x25) {
                std::memcpy(&displacement, bytes + 2, sizeof(displacement));
                std::memcpy(&entry, bytes + 6 + displacement, sizeof(entry));
            } else {
                return entry;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return nullptr;
}
bool ownMinHook = false;
thread_local bool drawingAlly = false, drawingLocal = false, replaying = false;
std::atomic<std::uint64_t> frameNumber{1};
std::uint64_t lastLog = 0;
int Kind(ID3D11DeviceContext* ctx) { return ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED ? 1 : 0; }
}
void SetDrawingAlly(bool value) noexcept { drawingAlly = value; }
bool IsDrawingAlly() noexcept { return drawingAlly; }
extern "C" {
__declspec(dllexport) volatile LONG ds3scOutlineShowMask = 0;
__declspec(dllexport) volatile LONG ds3scOutlineEnable = 0;
__declspec(dllexport) volatile LONG ds3scOutlineThicknessInt = 20;
__declspec(dllexport) volatile LONG ds3scOutlineFillSilhouette = 0;
__declspec(dllexport) volatile LONG ds3scOutlineVisible = 0;
__declspec(dllexport) volatile LONG ds3scOutlineFallbackMarkers = 1;
__declspec(dllexport) volatile LONG ds3scPlayerOutlineEnable = 0;
__declspec(dllexport) volatile LONG ds3scDiamondMarkersEnable = 1;
__declspec(dllexport) volatile LONG ds3scDisableVsync = 0;
__declspec(dllexport) volatile LONG ds3scD3D11Hooked = 0;
__declspec(dllexport) volatile LONG ds3scD3D11PresentCount = 0;
__declspec(dllexport) volatile LONG ds3scD3D11HookRepairs = 0;
__declspec(dllexport) volatile LONG ds3scAllyDrawsCount = 0;
__declspec(dllexport) volatile LONG ds3scLocalDrawsCount = 0;
__declspec(dllexport) volatile LONG ds3scAllyDispatchCalls = 0;
__declspec(dllexport) volatile LONG ds3scAllyModelCalls = 0;
__declspec(dllexport) volatile LONG ds3scAllyIndividualSubmissions = 0;
__declspec(dllexport) volatile LONG ds3scAllyOccludedSubmissions = 0;
__declspec(dllexport) volatile LONG ds3scAllyExecuteCalls = 0;
__declspec(dllexport) volatile LONG ds3scAllyMatchedDraws = 0;
__declspec(dllexport) volatile LONG ds3scAllyExecuteThread = 0;
__declspec(dllexport) volatile LONG ds3scAllyPacketCalls = 0;
__declspec(dllexport) volatile LONG ds3scAllyDeferredDraws = 0;
__declspec(dllexport) volatile LONG ds3scAllyCommandLists = 0;
__declspec(dllexport) volatile LONG ds3scAllyTraverserCalls = 0;
__declspec(dllexport) volatile LONG ds3scAllyBoundsPushed = 0;
__declspec(dllexport) volatile std::uintptr_t ds3scAllyLastPacket = 0;
__declspec(dllexport) volatile std::uintptr_t ds3scAllyLastPacketEntity = 0;
struct RenderTrace {
    LONG abi, modelThread, drawThread, totalDraws;
    std::uintptr_t modelEntity, modelContext, immediateContext;
    std::uintptr_t hookedTargets[4], actualTargets[4], drawStack[16];
};
__declspec(dllexport) volatile RenderTrace ds3scRenderTrace{1};
extern volatile LONG ds3scOutlineCaptureAttempts, ds3scOutlineCaptureRejectReason, ds3scOutlineComposeResult;
}
namespace {
#if DS3SC_FEATURE_OUTLINE_CAPTURE
struct AllyScope {
    bool previous = drawingAlly, previousLocal = drawingLocal;
    explicit AllyScope(bool ally, bool local = false) {
        drawingLocal = local;
        drawingAlly = ally && !local;
    }
    ~AllyScope() { drawingAlly = previous; drawingLocal = previousLocal; }
};
using ModelFn = void(*)(void*, void*);
using DispatchFn = void(*)(void*, void*, void*, void*);
using PacketFn = void(*)(void*, void*, void*, void*, void*);
using CollectTraverserAcceptFn = void(*)(void*, void*);
using TraverserPushBoundsFn = unsigned char(*)(void*, const void*);
using PushVectorFn = void(*)(void*, const void*);
ModelFn originalModel{};
ModelFn originalAsmModel{};
DispatchFn originalDispatch{};
PacketFn originalPacket{};
CollectTraverserAcceptFn originalCollectTraverserAccept{};
TraverserPushBoundsFn originalTraverserPushBounds{};
PushVectorFn originalPushVector{};

// GXSgCullTraverser::PushBounds (0x140234840) evaluates spatial nodes against
// the camera frustum and wall occlusion buffer. When an entire node is occluded
// by a wall, Culler->TestAABB returns 0 and the engine drops the entire node,
// never reaching CollectTraverserAccept or Model for the entities inside.
// If the node's AABB intersects any tracked ally and the engine returns 0,
// we execute the acceptance push, guaranteeing the node is visited.
unsigned char TraverserPushBounds(void* traverser, const void* bounds) {
    const unsigned char res = originalTraverserPushBounds(traverser, bounds);
    if (res) return res;

    if (bounds && ActorTracker::Instance().BoundsContainAlly(reinterpret_cast<const float*>(bounds))) {
        auto* t = reinterpret_cast<std::uint8_t*>(traverser);
        auto* vec = t + 0x50;
        auto* currentBounds = t + 0x30;

        if (originalPushVector) {
            originalPushVector(vec, currentBounds);
            std::memcpy(currentBounds, bounds, 32);
            *reinterpret_cast<volatile std::int32_t*>(t + 0x20) += 1;
            *reinterpret_cast<volatile std::int32_t*>(t + 0x24) += 1;
            InterlockedIncrement(&ds3scAllyBoundsPushed);
            return 1;
        }
    }
    return 0;
}

// GXSgEntityCollectTraverser::Accept (0x140142e80) evaluates whether to visit
// an entity for rendering. At distance or across portals, [r8+0x60] culls the entity.
// When traverser+0x24 > 0 and traverser+0x10 == 0, the engine branches unconditionally
// to 0x140142efa, bypassing all frustum, portal, and distance culling gates.
void CollectTraverserAccept(void* traverser, void* entity) {
    const auto address = reinterpret_cast<std::uintptr_t>(entity);
    const bool allyActive = (ds3scOutlineEnable != 0 || ds3scOutlineShowMask != 0 || ds3scDiamondMarkersEnable != 0 || ds3scOutlineFallbackMarkers != 0);
    const bool localActive = (ds3scPlayerOutlineEnable != 0 || ds3scDiamondMarkersEnable != 0 || ds3scOutlineFallbackMarkers != 0 || ds3scOutlineEnable != 0);
    const bool isAlly = allyActive && ActorTracker::Instance().IsDrawEntityTrackedAsAlly(address);
    const bool isLocal = localActive && ActorTracker::Instance().IsDrawEntityTrackedAsLocal(address);
    if (isAlly || isLocal) {
        InterlockedIncrement(&ds3scAllyTraverserCalls);
        auto* const t = reinterpret_cast<std::uint8_t*>(traverser);
        const auto t24Addr = reinterpret_cast<std::uintptr_t>(t + 0x24);
        std::int32_t prev24 = 0;
        const bool readOk24 = SafeRead(t24Addr, prev24);
        if (readOk24 && prev24 <= 0) {
            *reinterpret_cast<volatile std::int32_t*>(t24Addr) = 1;
        }
        const auto t10Addr = reinterpret_cast<std::uintptr_t>(t + 0x10);
        std::uint8_t prev10 = 0;
        const bool readOk10 = SafeRead(t10Addr, prev10);
        if (readOk10 && prev10 != 0) {
            *reinterpret_cast<volatile std::uint8_t*>(t10Addr) = 0;
        }

        // Override entity + 0x68 (GXVisbTester) if [r8+0x40] < 2 to prevent culling at 0x140142eb7
        std::uintptr_t tester = 0;
        std::int32_t prevTester40 = 0;
        bool modifiedTester40 = false;
        if (SafeRead(address + 0x68u, tester) && tester != 0) {
            const auto t40Addr = tester + 0x40u;
            if (SafeRead(t40Addr, prevTester40) && prevTester40 < 2) {
                *reinterpret_cast<volatile std::int32_t*>(t40Addr) = 2;
                modifiedTester40 = true;
            }
        }

        originalCollectTraverserAccept(traverser, entity);

        if (modifiedTester40 && tester != 0) {
            *reinterpret_cast<volatile std::int32_t*>(tester + 0x40u) = prevTester40;
        }
        if (readOk24 && prev24 <= 0) {
            *reinterpret_cast<volatile std::int32_t*>(t24Addr) = prev24;
        }
        if (readOk10 && prev10 != 0) {
            *reinterpret_cast<volatile std::uint8_t*>(t10Addr) = prev10;
        }
        return;
    }
    originalCollectTraverserAccept(traverser, entity);
}

// 59040 reads seven stack arguments (+0xd0 through +0x100 after its prologue).
using SubmitFn = void(*)(void*, void*, void*, void*, const void*, void*, void*, void*, void*, void*, float);
SubmitFn originalSubmit{};
void Submit(void* iface, void* context, void* resource, void* item, const void* options,
            void* material, void* extra, void* transform, void* bounds, void* center, float distance) {
    alignas(16) NativeSubmissionOptions individual{};
    const auto ifaceAddr = reinterpret_cast<std::uintptr_t>(iface);
    const bool isAlly = ActorTracker::Instance().IsDrawEntityTrackedAsAlly(ifaceAddr);
    const bool isLocal = ActorTracker::Instance().IsDrawEntityTrackedAsLocal(ifaceAddr);
    const bool allyActive = (ds3scOutlineEnable != 0 || ds3scOutlineShowMask != 0 || ds3scDiamondMarkersEnable != 0 || ds3scOutlineFallbackMarkers != 0);
    const bool localActive = (ds3scPlayerOutlineEnable != 0 || ds3scDiamondMarkersEnable != 0 || ds3scOutlineFallbackMarkers != 0 || ds3scOutlineEnable != 0);
    const bool shouldSeparate = (isAlly && allyActive) || (isLocal && localActive);
    if (shouldSeparate && IndividualAllySubmission(options, individual)) {
        options = individual.data();
        if (isAlly) InterlockedIncrement(&ds3scAllyIndividualSubmissions);
    }
    originalSubmit(iface, context, resource, item, options, material, extra, transform, bounds, center, distance);
}

// Lock-free tracker ensuring every tracked ally is submitted to Pass 8
// (the main scene camera pass) at least once and at most once per frame.
// Even if distant scene graph culling, portal occlusion, or frustum gates
// prevent the engine's traverser from visiting the ally, our Pass 8 fallback
// submits the ally using the active Pass 8 context, guaranteeing continuous,
// rock-solid outline visibility through walls and at arbitrary distances.
struct AllyPass8Tracker {
    static constexpr std::size_t kMaxAllies = 32;
    struct Entry {
        std::atomic<std::uintptr_t> entity{0};
        std::atomic<std::uint64_t> lastFrame{0};
    };
    Entry entries[kMaxAllies];
    std::atomic<std::uint64_t> lastAllSubmittedFrame{0};

    // Returns true if this is the first submission of 'allyEntity' in Pass 8 for 'currentFrame'.
    bool TryClaim(std::uintptr_t allyEntity, std::uint64_t currentFrame) noexcept {
        if (!allyEntity) return false;

        Entry* target = nullptr;
        for (std::size_t i = 0; i < kMaxAllies; ++i) {
            const std::uintptr_t e = entries[i].entity.load(std::memory_order_relaxed);
            if (e == allyEntity) {
                target = &entries[i];
                break;
            }
            if (e == 0 && !target) {
                target = &entries[i];
            }
        }

        if (!target) return false;

        std::uintptr_t expectedEntity = 0;
        if (target->entity.load(std::memory_order_relaxed) == 0) {
            if (!target->entity.compare_exchange_strong(expectedEntity, allyEntity, std::memory_order_acq_rel)) {
                if (expectedEntity != allyEntity) {
                    return TryClaim(allyEntity, currentFrame);
                }
            }
        }

        std::uint64_t prev = target->lastFrame.load(std::memory_order_relaxed);
        while (prev < currentFrame) {
            if (target->lastFrame.compare_exchange_weak(prev, currentFrame, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void Reset() noexcept {
        lastAllSubmittedFrame.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < kMaxAllies; ++i) {
            entries[i].entity.store(0, std::memory_order_relaxed);
            entries[i].lastFrame.store(0, std::memory_order_relaxed);
        }
    }
};

static AllyPass8Tracker g_pass8Tracker;

void AsmModel(void* entity, void* context);

inline void CallEntityOriginalModel(void* entity, void* context) {
    if (!entity || !context) return;
    const auto address = reinterpret_cast<std::uintptr_t>(entity);
    std::uintptr_t vtable = 0;
    if (!SafeRead(address, vtable) || !vtable) return;

    std::uintptr_t slot1 = 0;
    if (!SafeRead(vtable + 8u, slot1) || !slot1) return;

    const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (slot1 == gameBase + 0xd00200 || slot1 == reinterpret_cast<std::uintptr_t>(&AsmModel) ||
        vtable == gameBase + 0x2947db8) {
        if (originalAsmModel) originalAsmModel(entity, context);
    } else {
        if (originalModel) originalModel(entity, context);
    }
}

void Model(void* entity, void* context) {
    const auto address = reinterpret_cast<std::uintptr_t>(entity);
    const bool isLocal = ActorTracker::Instance().IsDrawEntityTrackedAsLocal(address);
    const bool isAlly = !isLocal && ActorTracker::Instance().IsDrawEntityTrackedAsAlly(address);
    const auto currentFrame = frameNumber.load(std::memory_order_relaxed);

    if (isAlly || isLocal) {
        InterlockedIncrement(&ds3scAllyModelCalls);
        ds3scRenderTrace.modelThread = GetCurrentThreadId();
        ds3scRenderTrace.modelEntity = address;
        ds3scRenderTrace.modelContext = reinterpret_cast<std::uintptr_t>(context);

        const bool allyOutlineActive = (ds3scOutlineEnable != 0 || ds3scOutlineShowMask != 0);
        const bool playerOutlineActive = (ds3scPlayerOutlineEnable != 0);
        const bool markersActive = (ds3scDiamondMarkersEnable != 0 || ds3scOutlineFallbackMarkers != 0);
        const bool localActive = (playerOutlineActive || markersActive || allyOutlineActive);

        if ((isAlly && allyOutlineActive) || (isLocal && localActive)) {
            std::uint32_t pass = 0;
            SafeRead(reinterpret_cast<std::uintptr_t>(context) + 0x124, pass);

            if (pass == 8 && isAlly) {
                // If this ally was already claimed/submitted in Pass 8 for this frame,
                // do not submit duplicate geometry.
                if (!g_pass8Tracker.TryClaim(address, currentFrame)) {
                    return;
                }
            }

            std::lock_guard<std::recursive_mutex> lock(allyModelMutex);
            AllyScope allyScope(isAlly, isLocal);
            ScopedAllyModelOverride visibility(address, pass);
            if (visibility.Changed() && isAlly) InterlockedIncrement(&ds3scAllyOccludedSubmissions);
            originalModel(entity, context);
            return;
        }

        originalModel(entity, context);
        return;
    }

    // entity is NOT an ally or local
    const bool allyOutlineActive = (ds3scOutlineEnable != 0 || ds3scOutlineShowMask != 0);

    if (allyOutlineActive) {
        std::uint32_t pass = 0;
        if (SafeRead(reinterpret_cast<std::uintptr_t>(context) + 0x124, pass) && pass == 8) {
            std::array<std::uintptr_t, ActorTracker::kMaxFastAllies> allies{};
            const auto allyCount = ActorTracker::Instance().GetFastAllyEntities(allies.data(), allies.size());

            for (std::size_t i = 0; i < allyCount; ++i) {
                const auto allyAddr = allies[i];
                if (!allyAddr) continue;

                if (g_pass8Tracker.TryClaim(allyAddr, currentFrame)) {
                    std::lock_guard<std::recursive_mutex> lock(allyModelMutex);
                    AllyScope allyScope(true, false);
                    ScopedAllyModelOverride visibility(allyAddr, 8);
                    if (visibility.Changed()) InterlockedIncrement(&ds3scAllyOccludedSubmissions);
                    InterlockedIncrement(&ds3scAllyModelCalls);
                    ds3scRenderTrace.modelThread = GetCurrentThreadId();
                    ds3scRenderTrace.modelEntity = allyAddr;
                    ds3scRenderTrace.modelContext = reinterpret_cast<std::uintptr_t>(context);
                    CallEntityOriginalModel(reinterpret_cast<void*>(allyAddr), context);
                }
            }
        }
    }

    originalModel(entity, context);
}

void AsmModel(void* entity, void* context) {
    const auto address = reinterpret_cast<std::uintptr_t>(entity);
    const bool isLocal = ActorTracker::Instance().IsDrawEntityTrackedAsLocal(address);
    const bool isAlly = !isLocal && ActorTracker::Instance().IsDrawEntityTrackedAsAlly(address);
    const auto currentFrame = frameNumber.load(std::memory_order_relaxed);

    if (isAlly || isLocal) {
        InterlockedIncrement(&ds3scAllyModelCalls);
        ds3scRenderTrace.modelThread = GetCurrentThreadId();
        ds3scRenderTrace.modelEntity = address;
        ds3scRenderTrace.modelContext = reinterpret_cast<std::uintptr_t>(context);

        const bool allyOutlineActive = (ds3scOutlineEnable != 0 || ds3scOutlineShowMask != 0);
        const bool playerOutlineActive = (ds3scPlayerOutlineEnable != 0);
        const bool markersActive = (ds3scDiamondMarkersEnable != 0 || ds3scOutlineFallbackMarkers != 0);
        const bool localActive = (playerOutlineActive || markersActive || allyOutlineActive);

        if ((isAlly && allyOutlineActive) || (isLocal && localActive)) {
            std::uint32_t pass = 0;
            SafeRead(reinterpret_cast<std::uintptr_t>(context) + 0x124, pass);

            if (pass == 8 && isAlly) {
                // If this ally was already claimed/submitted in Pass 8 for this frame,
                // do not submit duplicate geometry.
                if (!g_pass8Tracker.TryClaim(address, currentFrame)) {
                    return;
                }
            }

            std::lock_guard<std::recursive_mutex> lock(allyModelMutex);
            AllyScope allyScope(isAlly, isLocal);
            ScopedAllyModelOverride visibility(address, pass);
            if (visibility.Changed() && isAlly) InterlockedIncrement(&ds3scAllyOccludedSubmissions);
            originalAsmModel(entity, context);
            return;
        }

        originalAsmModel(entity, context);
        return;
    }

    // entity is NOT an ally or local
    const bool allyOutlineActive = (ds3scOutlineEnable != 0 || ds3scOutlineShowMask != 0);

    if (allyOutlineActive) {
        std::uint32_t pass = 0;
        if (SafeRead(reinterpret_cast<std::uintptr_t>(context) + 0x124, pass) && pass == 8) {
            std::array<std::uintptr_t, ActorTracker::kMaxFastAllies> allies{};
            const auto allyCount = ActorTracker::Instance().GetFastAllyEntities(allies.data(), allies.size());

            for (std::size_t i = 0; i < allyCount; ++i) {
                const auto allyAddr = allies[i];
                if (!allyAddr) continue;

                if (g_pass8Tracker.TryClaim(allyAddr, currentFrame)) {
                    std::lock_guard<std::recursive_mutex> lock(allyModelMutex);
                    AllyScope allyScope(true, false);
                    ScopedAllyModelOverride visibility(allyAddr, 8);
                    if (visibility.Changed()) InterlockedIncrement(&ds3scAllyOccludedSubmissions);
                    InterlockedIncrement(&ds3scAllyModelCalls);
                    ds3scRenderTrace.modelThread = GetCurrentThreadId();
                    ds3scRenderTrace.modelEntity = allyAddr;
                    ds3scRenderTrace.modelContext = reinterpret_cast<std::uintptr_t>(context);
                    CallEntityOriginalModel(reinterpret_cast<void*>(allyAddr), context);
                }
            }
        }
    }

    originalAsmModel(entity, context);
}

void Dispatch(void* iface, void* context, void* cmd, void* arg4) {
    // The dispatch interface is per instance, including the alternate command
    // pair. Shared FLVER resources and arbitrary cmd offsets are not identity.
    const auto ifaceAddr = reinterpret_cast<std::uintptr_t>(iface);
    const bool isLocal = ActorTracker::Instance().IsDrawEntityTrackedAsLocal(ifaceAddr);
    const bool ally = !isLocal && ActorTracker::Instance().IsDrawEntityTrackedAsAlly(ifaceAddr);
    AllyScope scope(ally, isLocal);
    if (ally) {
        InterlockedIncrement(&ds3scAllyDispatchCalls);
        ds3scAllyExecuteThread = GetCurrentThreadId();
    }
    originalDispatch(iface, context, cmd, arg4);
}

void Packet(void* context, void* model, void* material, void* packet, void* parameters) {
    InterlockedIncrement(&ds3scAllyPacketCalls);
    const auto address = reinterpret_cast<std::uintptr_t>(packet);
    const auto entity = PacketDrawEntity(address);
    if (entity != 0) {
        const bool isLocal = ActorTracker::Instance().IsDrawEntityTrackedAsLocal(entity);
        const bool ally = !isLocal && ActorTracker::Instance().IsDrawEntityTrackedAsAlly(entity);
        AllyScope scope(ally, isLocal);
        if (ally) {
            ds3scAllyLastPacket = address;
            ds3scAllyLastPacketEntity = entity;
            InterlockedIncrement(&ds3scAllyExecuteCalls);
            ds3scAllyExecuteThread = GetCurrentThreadId();
        }
        originalPacket(context, model, material, packet, parameters);
    } else {
        if (drawingAlly) {
            ds3scAllyLastPacket = address;
            InterlockedIncrement(&ds3scAllyExecuteCalls);
            ds3scAllyExecuteThread = GetCurrentThreadId();
        }
        originalPacket(context, model, material, packet, parameters);
    }
}

void Trace() {
    if (replaying) return;
    if (drawingAlly) InterlockedIncrement(&ds3scAllyMatchedDraws);
    if (InterlockedIncrement(&ds3scRenderTrace.totalDraws) == 1) {
        ds3scRenderTrace.drawThread = GetCurrentThreadId();
        void* stack[16]{};
        const auto count = CaptureStackBackTrace(0, 16, stack, nullptr);
        for (USHORT i = 0; i < count; ++i) ds3scRenderTrace.drawStack[i] = reinterpret_cast<std::uintptr_t>(stack[i]);
    }
}

void Capture(ID3D11DeviceContext* ctx, const ReplayDrawParams& params, void* original) {
    if (replaying) return;
    if (drawingAlly && Kind(ctx)) InterlockedIncrement(&ds3scAllyDeferredDraws);

    const bool allyOutlineActive = (ds3scOutlineEnable != 0 || ds3scOutlineShowMask != 0);
    const bool playerOutlineActive = (ds3scPlayerOutlineEnable != 0);
    const bool markersActive = (ds3scDiamondMarkersEnable != 0 || ds3scOutlineFallbackMarkers != 0);

    if (drawingAlly && !allyOutlineActive) return;
    if (drawingLocal && !playerOutlineActive && !markersActive && !allyOutlineActive) return;
    if (!drawingAlly && !drawingLocal) return;

    replaying = true;
    if (LiveAllyOutline::Instance().Capture(ctx, params, original, drawingLocal)) {
        if (drawingLocal) InterlockedIncrement(&ds3scLocalDrawsCount);
        else InterlockedIncrement(&ds3scAllyDrawsCount);
    }
    replaying = false;
}

void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* ctx, UINT count, UINT start) {
    Trace(); const auto fn = originalDraw[Kind(ctx)]; fn(ctx, count, start);
    ReplayDrawParams p{}; p.type = DrawCallType::Draw; p.vertexCount = count; p.startVertexLocation = start;
    Capture(ctx, p, reinterpret_cast<void*>(fn));
}

void STDMETHODCALLTYPE Indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base) {
    Trace(); const auto fn = originalIndexed[Kind(ctx)]; fn(ctx, count, start, base);
    ReplayDrawParams p{}; p.type = DrawCallType::DrawIndexed; p.indexCount = count;
    p.startIndexLocation = start; p.baseVertexLocation = base;
    Capture(ctx, p, reinterpret_cast<void*>(fn));
}
void STDMETHODCALLTYPE Instanced(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, UINT first) {
    Trace(); const auto fn = originalInstanced[Kind(ctx)]; fn(ctx, count, instances, start, first);
    ReplayDrawParams p{}; p.type = DrawCallType::DrawInstanced; p.vertexCount = count;
    p.instanceCount = instances; p.startVertexLocation = start; p.startInstanceLocation = first;
    Capture(ctx, p, reinterpret_cast<void*>(fn));
}
void STDMETHODCALLTYPE IndexedInstanced(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base, UINT first) {
    Trace(); const auto fn = originalIndexedInstanced[Kind(ctx)]; fn(ctx, count, instances, start, base, first);
    ReplayDrawParams p{}; p.type = DrawCallType::DrawIndexedInstanced; p.indexCount = count;
    p.instanceCount = instances; p.startIndexLocation = start; p.baseVertexLocation = base; p.startInstanceLocation = first;
    Capture(ctx, p, reinterpret_cast<void*>(fn));
}
void STDMETHODCALLTYPE Clear(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil) {
    LiveAllyOutline::Instance().BeforeDepthClear(ctx, view, flags, depth);
    originalClear[Kind(ctx)](ctx, view, flags, depth, stencil);
}
HRESULT STDMETHODCALLTYPE Finish(ID3D11DeviceContext* ctx, BOOL restore, ID3D11CommandList** list) {
    const auto hr = originalFinish(ctx, restore, list);
    LiveAllyOutline::Instance().Finish(ctx, SUCCEEDED(hr) && list ? *list : nullptr);
    return hr;
}
void STDMETHODCALLTYPE Execute(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore) {
    originalExecute(ctx, list, restore);
    if (LiveAllyOutline::Instance().Executed(list)) InterlockedIncrement(&ds3scAllyCommandLists);
}
#endif // DS3SC_FEATURE_OUTLINE_CAPTURE

void Log() {
    const auto now = GetTickCount64();
    if (now - lastLog < 3000) return;
    lastLog = now;
    char line[256]{};
    std::snprintf(line, sizeof(line),
        "[ds3sc_render] frame=%llu playerOutline=%ld localDraws=%ld allyDraws=%ld localCaptures=%u\n",
        static_cast<unsigned long long>(frameNumber.load(std::memory_order_relaxed)),
        ds3scPlayerOutlineEnable,
        ds3scLocalDrawsCount,
        ds3scAllyDrawsCount,
#if DS3SC_FEATURE_OUTLINE_CAPTURE
        LiveAllyOutline::Instance().GetLocalCaptures());
#else
        0u);
#endif
    OutputDebugStringA(line);
}
HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return originalPresent(swap, sync, flags);
    InterlockedIncrement(&ds3scD3D11PresentCount);
    const auto currentFrame = frameNumber.fetch_add(1, std::memory_order_acq_rel) + 1;
    ds3scD3D11Hooked = 1;
    if (!ds3scRenderTrace.immediateContext) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&device)))) {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx; device->GetImmediateContext(&ctx);
            ds3scRenderTrace.immediateContext = reinterpret_cast<std::uintptr_t>(ctx.Get());
            auto** table = *reinterpret_cast<void***>(ctx.Get()); const int slots[]{12,13,20,21};
            for (int i=0;i<4;++i) ds3scRenderTrace.actualTargets[i] = reinterpret_cast<std::uintptr_t>(table[slots[i]]);
        }
    }
#if DS3SC_FEATURE_OUTLINE_CAPTURE || (defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS)
    ActorTracker::Instance().Update(currentFrame);
#else
    (void)currentFrame;
#endif

#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
    LiveAllyMarkers::Instance().Present(swap, ds3scDiamondMarkersEnable != 0 || ds3scOutlineFallbackMarkers != 0);
#endif

#if DS3SC_FEATURE_OUTLINE_CAPTURE
    const bool allyOutlineActive = (ds3scOutlineEnable != 0 || ds3scOutlineShowMask != 0);
    LiveAllyOutline::Instance().Present(swap, allyOutlineActive,
        ds3scOutlineShowMask != 0, ds3scOutlineThicknessInt / 10.0f, ds3scOutlineVisible != 0,
        ds3scOutlineFillSilhouette != 0, .30f, ds3scOutlineFallbackMarkers != 0, ds3scPlayerOutlineEnable != 0);
    g_pass8Tracker.Reset();
#endif

#if (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS) || (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS)
    CombatStatsOverlay::Instance().Present(swap);
#endif
    TitleMenu::Instance().Present(swap);
    Log();
    if (ds3scDisableVsync != 0) {
        sync = 0;
    }
    return originalPresent(swap, sync, flags);
}
HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
#if DS3SC_FEATURE_OUTLINE_CAPTURE
    LiveAllyOutline::Instance().Reset();
    g_pass8Tracker.Reset();
#endif
#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
    LiveAllyMarkers::Instance().Reset();
#endif
#if (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS) || (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS)
    CombatStatsOverlay::Instance().Reset();
#endif
    TitleMenu::Instance().Reset();
    return originalResize(swap, count, width, height, format, flags);
}
struct Hook { void* target; void* replacement; void** original; };
void RemoveOwnedHooks() {
    for (auto* target : ownedHooks) MH_DisableHook(target);
    for (auto* target : ownedHooks) MH_RemoveHook(target);
    ownedHooks.clear();
    presentationHooks = {};
    if (ownMinHook) MH_Uninitialize();
    ownMinHook = false;
}
}
D3D11HookManager& D3D11HookManager::Instance() noexcept { static D3D11HookManager value; return value; }
void D3D11HookManager::MaintainPresentationHooks() noexcept {
    // Called on the extension worker, never from inside a Present detour.
    std::lock_guard<std::mutex> lock(installMutex);
    if (!installed_.load()) return;
    for (const auto& hook : presentationHooks) {
        std::array<unsigned char, 5> current{};
        if (!ReadPresentationEntry(hook.target, current)) continue;
        // Some overlays restore the exact pre-hook entry during late startup.
        // MinHook still thinks our detour is enabled in that case. Repair only
        // that exact restoration: an unknown third-party patch is left alone.
        if (current != hook.originalBytes) continue;
        const auto disabled = MH_DisableHook(hook.target);
        if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) continue;
        if (MH_EnableHook(hook.target) == MH_OK) {
            InterlockedIncrement(&ds3scD3D11HookRepairs);
            OutputDebugStringA("[ds3sc-render] Restored presentation detour after original entry was reinstated.\n");
        }
    }
}
bool D3D11HookManager::Install() noexcept {
    std::lock_guard<std::mutex> lock(installMutex);
    if (installed_.load()) return true;
    HWND window = CreateWindowExW(0, L"STATIC", L"Outline device", WS_OVERLAPPEDWINDOW,
        0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (!window) return false;
    DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferCount=1; desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow=window; desc.SampleDesc.Count=1; desc.Windowed=TRUE;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> contexts[2];
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&contexts[0]);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&contexts[0]);
    if (FAILED(hr) || FAILED(device->CreateDeferredContext(0, &contexts[1]))) { DestroyWindow(window); return false; }
    auto** table = *reinterpret_cast<void***>(swap.Get());
    void* presentEntry = ResolvePresentationEntry(table[8]);
    void* resizeEntry = ResolvePresentationEntry(table[13]);
    if (!presentEntry || !resizeEntry) { DestroyWindow(window); return false; }
    std::vector<Hook> hooks{
        {presentEntry,reinterpret_cast<void*>(&Present),reinterpret_cast<void**>(&originalPresent)},
        {resizeEntry,reinterpret_cast<void*>(&Resize),reinterpret_cast<void**>(&originalResize)}};
#if DS3SC_FEATURE_OUTLINE_CAPTURE
    for (int k=0;k<2;++k) {
        table = *reinterpret_cast<void***>(contexts[k].Get());
        hooks.push_back({table[12],reinterpret_cast<void*>(&Indexed),reinterpret_cast<void**>(&originalIndexed[k])});
        hooks.push_back({table[13],reinterpret_cast<void*>(&Draw),reinterpret_cast<void**>(&originalDraw[k])});
        hooks.push_back({table[20],reinterpret_cast<void*>(&IndexedInstanced),reinterpret_cast<void**>(&originalIndexedInstanced[k])});
        hooks.push_back({table[21],reinterpret_cast<void*>(&Instanced),reinterpret_cast<void**>(&originalInstanced[k])});
        hooks.push_back({table[53],reinterpret_cast<void*>(&Clear),reinterpret_cast<void**>(&originalClear[k])});
        if (k) hooks.push_back({table[114],reinterpret_cast<void*>(&Finish),reinterpret_cast<void**>(&originalFinish)});
        else {
            hooks.push_back({table[58],reinterpret_cast<void*>(&Execute),reinterpret_cast<void**>(&originalExecute)});
            const int slots[]{12,13,20,21};
            for (int i=0;i<4;++i) ds3scRenderTrace.hookedTargets[i] = reinterpret_cast<std::uintptr_t>(table[slots[i]]);
        }
    }
    bool ready = true;
    const auto game = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"DarkSoulsIII.exe"));
    if (game) {
        auto verify = [&](std::uintptr_t rva, const auto& prefix) {
            std::array<unsigned char,32> bytes{};
            return SafeReadBytes(game+rva,bytes.data(),prefix.size()) && !std::memcmp(bytes.data(),prefix.data(),prefix.size());
        };
        const std::array<unsigned char, 13> modelPrefix{0x48,0x8b,0xc4,0x55,0x56,0x57,0x48,0x81,0xec,0x30,0x02,0x00,0x00};
        const std::array<unsigned char, 13> asmModelPrefix{0x48,0x8b,0xc4,0x55,0x56,0x57,0x48,0x81,0xec,0x30,0x02,0x00,0x00};
        const std::array<unsigned char, 22> dispatchPrefix{0x40,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8d,0xac,0x24,0xb0,0xf9,0xff,0xff,0x48,0x81,0xec,0x50,0x07,0x00,0x00};
        const std::array<unsigned char, 28> packetPrefix{0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x88,0xf8,0xff,0xff,0x48,0x81,0xec,0x78,0x08,0,0};
        const std::array<unsigned char, 15> submitPrefix{0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x70};
        const std::array<unsigned char, 8> passRangePrefix{0x8d,0x41,0xfb,0x83,0xf8,0x03,0x77,0x1d};
        const std::array<unsigned char, 18> passCullPrefix{0x80,0xbb,0x5d,0x0a,0,0,0,0x0f,0x94,0xc0,0x84,0xc0,0x0f,0x84,0x22,0x01,0,0};
        const std::array<unsigned char, 16> collectAcceptPrefix{0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x01,0x48,0x8b,0xfa};
        const std::array<unsigned char, 15> pushBoundsPrefix{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20};
        const std::array<unsigned char, 14> pushVecPrefix{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,0x08};
        std::uintptr_t entryModel{}, entryAsmModel{}, entryDispatch{}, entryAccept{}, entryPushBounds{};
        ready = SafeRead(game+0x2948f00,entryModel) && entryModel==game+0xd06e10 &&
                SafeRead(game+0x2947dc0,entryAsmModel) && entryAsmModel==game+0xd00200 &&
                SafeRead(game+0x2949088,entryDispatch) && entryDispatch==game+0x59220 &&
                SafeRead(game+0x3d529d8,entryAccept) && entryAccept==game+0x142e80 &&
                SafeRead(game+0x3d529a0,entryPushBounds) && entryPushBounds==game+0x234840 &&
                verify(0xd06e10,modelPrefix) &&
                verify(0xd00200,asmModelPrefix) &&
                verify(0x59220,dispatchPrefix) &&
                verify(0x5cec0,packetPrefix) && verify(0x59040,submitPrefix) &&
                verify(0xd06f3f,passRangePrefix) && verify(0xd06f50,passCullPrefix) &&
                verify(0x142e80,collectAcceptPrefix) &&
                verify(0x234840,pushBoundsPrefix) &&
                verify(0x19c130,pushVecPrefix);
        originalPushVector = reinterpret_cast<PushVectorFn>(game + 0x19c130);
        hooks.push_back({reinterpret_cast<void*>(game+0xd06e10),reinterpret_cast<void*>(&Model),reinterpret_cast<void**>(&originalModel)});
        hooks.push_back({reinterpret_cast<void*>(game+0xd00200),reinterpret_cast<void*>(&AsmModel),reinterpret_cast<void**>(&originalAsmModel)});
        hooks.push_back({reinterpret_cast<void*>(game+0x59220),reinterpret_cast<void*>(&Dispatch),reinterpret_cast<void**>(&originalDispatch)});
        hooks.push_back({reinterpret_cast<void*>(game+0x5cec0),reinterpret_cast<void*>(&Packet),reinterpret_cast<void**>(&originalPacket)});
        hooks.push_back({reinterpret_cast<void*>(game+0x59040),reinterpret_cast<void*>(&Submit),reinterpret_cast<void**>(&originalSubmit)});
        hooks.push_back({reinterpret_cast<void*>(game+0x142e80),reinterpret_cast<void*>(&CollectTraverserAccept),reinterpret_cast<void**>(&originalCollectTraverserAccept)});
        hooks.push_back({reinterpret_cast<void*>(game+0x234840),reinterpret_cast<void*>(&TraverserPushBounds),reinterpret_cast<void**>(&originalTraverserPushBounds)});}
#else
    bool ready = true;
#endif
    if (ready) { const auto status=MH_Initialize(); ownMinHook=status==MH_OK; ready=ownMinHook || status==MH_ERROR_ALREADY_INITIALIZED; }
    if (ready) for (size_t i=0;i<hooks.size();++i) {
        auto& hook=hooks[i]; bool duplicate=false;
        for (size_t j=0;j<i;++j) if (hooks[j].target==hook.target) {
            *hook.original=*hooks[j].original; duplicate=true; break;
        }
        if (duplicate) continue;
        if (i < presentationHooks.size()) {
            auto& presentation = presentationHooks[i];
            if (!ReadPresentationEntry(hook.target, presentation.originalBytes)) {
                ready = false;
                break;
            }
            presentation.target = hook.target;
        }
        if (MH_CreateHook(hook.target,hook.replacement,hook.original)!=MH_OK) { ready=false; break; }
        ownedHooks.push_back(hook.target);
    }
    if (ready) for (auto* target:ownedHooks) if (MH_EnableHook(target)!=MH_OK) { ready=false; break; }
    contexts[1].Reset(); contexts[0].Reset(); swap.Reset(); device.Reset(); DestroyWindow(window);
    if (!ready) { RemoveOwnedHooks(); return false; }
    installed_=true; return true;
}
void D3D11HookManager::Uninstall() noexcept {
    std::lock_guard<std::mutex> lock(installMutex);
    if (!installed_.load()) return;
    RemoveOwnedHooks();
#if DS3SC_FEATURE_OUTLINE_CAPTURE
    LiveAllyOutline::Instance().Reset();
#endif
#if defined(DS3SC_FEATURE_ALLY_MARKERS) && DS3SC_FEATURE_ALLY_MARKERS
    LiveAllyMarkers::Instance().Reset();
#endif
#if (defined(DS3SC_FEATURE_CONTADORES) && DS3SC_FEATURE_CONTADORES) || (defined(DS3SC_FEATURE_COMBAT_STATS) && DS3SC_FEATURE_COMBAT_STATS) || (defined(DS3SC_FEATURE_COUNTERS) && DS3SC_FEATURE_COUNTERS)
    CombatStatsOverlay::Instance().Reset();
#endif
    TitleMenu::Instance().Reset();
    ds3scD3D11Hooked=0; installed_=false;
}
} // namespace ds3sc::render
