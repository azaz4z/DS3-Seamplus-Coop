// Optional, local diagnostic companion for the verified DS3 1.15.2 co-op build.
// The original mod owns the callbacks. Swap their aligned target pointers rather
// than overwriting instructions. Actor creation/removal runs on the native debug
// manager's update thread. The outline hooks install on a separate worker.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>

#include "../render/d3d11_hook.h"
#include "../render/ally_outline.h"
#include "../render/actor_tracker.h"

namespace {
using Address = std::uintptr_t;
constexpr std::uint32_t kGoods = 589006;
constexpr std::int32_t kAction = -101;
constexpr Address kWorld = 0x477fdb8;
constexpr Address kDebugVtable = 0x28a4478;
constexpr Address kDebugUpdate = 0xa10e10;
constexpr Address kSpawn = 0xa122f0;
constexpr Address kRemove = 0x8dbc30;
struct Lookup { std::uint32_t id = ~0u; std::uint32_t type = ~0u; void* row = nullptr; };
using GoodsLookup = void (*)(Address, Lookup*, std::uint32_t);
using TextLookup = const wchar_t* (*)(Address, Address, std::uint32_t, int, int);
using ItemUse = void (*)(Address, Address, int, Address, std::uint32_t, Address, std::uint32_t, std::uint8_t, std::uint32_t, std::uint8_t);
using CanUse = std::uint64_t (*)(Address, std::uint32_t, Address, Address, std::uint32_t, std::uint32_t, std::uint32_t);
using Update = void (*)(Address, Address);
struct Binding { Address rva; void* replacement; void** slot = nullptr; void* original = nullptr; };
Address game = 0, mod = 0, owner = 0;
std::atomic_bool enabled = false;
alignas(16) std::array<std::byte, 0x80> goodsRow{};
std::atomic_bool rowReady = false;
std::atomic_bool toggleRequested = false;
Address activeWorld = 0, activeLocal = 0, actor = 0;
bool spawning = false;
ULONGLONG started = 0, lastGiftCheck = 0, lastUse = 0;
constexpr std::array<Address, 6> kSettingOffsets{0x124, 0x128, 0x1b0, 0x1b4, 0x1b8, 0x1bc};
std::array<std::uint32_t, 6> savedDebugSettings{};
Address settingsOwner = 0;
Update originalUpdate = nullptr;

bool Readable(Address address, std::size_t size) {
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !size || address + size < address || !VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) return false;
    return region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && address + size <= reinterpret_cast<Address>(region.BaseAddress) + region.RegionSize;
}
template<class T> T Read(Address address) {
    T value{};
    if (Readable(address, sizeof(T))) {
        __try { std::memcpy(&value, reinterpret_cast<void*>(address), sizeof(T)); }
        __except(EXCEPTION_EXECUTE_HANDLER) { value = {}; }
    }
    return value;
}
template<class T> void Write(Address address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}
void LookupGoods(Address self, Lookup* result, std::uint32_t id);
const wchar_t* LookupText(Address self, Address context, std::uint32_t language, int category, int id);
void UseItem(Address self, Address player, int action, Address a, std::uint32_t b, Address c, std::uint32_t d, std::uint8_t e, std::uint32_t f, std::uint8_t g);
std::uint64_t ItemAvailable(Address self, std::uint32_t id, Address a, Address b, std::uint32_t c, std::uint32_t d, std::uint32_t e);
void Tick(Address manager, Address dt);
std::array<Binding, 4> bindings{{
    {0x49170, reinterpret_cast<void*>(&LookupGoods)},
    {0x496b0, reinterpret_cast<void*>(&LookupText)},
    {0x4faa0, reinterpret_cast<void*>(&UseItem)},
    {0x4fbc0, reinterpret_cast<void*>(&ItemAvailable)}
}};
}

// Readable by the test harness. Commands: 1 toggle NPC, 2 retry gift, 3 disable.
// State: 0 not installed, 1 ready, 2 loading NPC, 3 NPC alive, 4 disabled, 5 failed.
extern "C" {
struct CompanionStatus {
    std::uint32_t abi, state, error, giftCount;
    std::uint64_t actor, model, updates, uses, drawEntity;
};
__declspec(dllexport) volatile CompanionStatus ds3scCompanionStatus{1, 0, 0, 0, 0, 0, 0, 0, 0};
__declspec(dllexport) volatile LONG ds3scCompanionCommand = 0;
}

namespace {
void LookupGoods(Address self, Lookup* result, std::uint32_t id) {
    if (id == kGoods && rowReady.load(std::memory_order_acquire)) {
        *result = {kGoods, 2, goodsRow.data()};
        return;
    }
    reinterpret_cast<GoodsLookup>(bindings[0].original)(self, result, id);
}
const wchar_t* LookupText(Address self, Address context, std::uint32_t language, int category, int id) {
    if (id == static_cast<int>(kGoods)) {
        if (category == 10) return L"Ash Stone (Summon Companion)";
        if (category == 20) return L"Summons or dismisses a local test ally NPC.";
        if (category == 24) return L"A magical ash stone for testing.\nSummons a single ally NPC near the bearer.\nUse again to dismiss. Solo only.";
    }
    return reinterpret_cast<TextLookup>(bindings[1].original)(self, context, language, category, id);
}
bool Solo(Address world) {
    const auto context = Read<Address>(owner + 0x28);
    const auto state = Read<Address>(context + 0xb0);
    return world && state && !(Read<std::uint32_t>(state) & 1u) && Read<Address>(world + 0x80);
}
void UseItem(Address self, Address player, int action, Address a, std::uint32_t b, Address c, std::uint32_t d, std::uint8_t e, std::uint32_t f, std::uint8_t g) {
    if (action == kAction) {
        const auto world = Read<Address>(game + kWorld);
        if (enabled.load() && Solo(world) && player == Read<Address>(world + 0x80)) {
            toggleRequested.store(true);
            ++ds3scCompanionStatus.uses;
        }
        return;
    }
    reinterpret_cast<ItemUse>(bindings[2].original)(self, player, action, a, b, c, d, e, f, g);
}
std::uint64_t ItemAvailable(Address self, std::uint32_t id, Address a, Address b, std::uint32_t c, std::uint32_t d, std::uint32_t e) {
    if (id == kGoods) return enabled.load() && Solo(Read<Address>(game + kWorld)) && rowReady.load();
    return reinterpret_cast<CanUse>(bindings[3].original)(self, id, a, b, c, d, e);
}
bool InDebugList(Address world, Address candidate) {
    const auto begin = Read<Address>(world + 0x2ff8), end = Read<Address>(world + 0x3000);
    if (!candidate || !begin || end < begin || (end - begin) % 8 || end - begin > 512 * 8) return false;
    for (auto entry = begin; entry != end; entry += 8) if (Read<Address>(entry) == candidate) return true;
    return false;
}
void RestoreSettings(Address manager) {
    if (settingsOwner == manager) {
        for (std::size_t i = 0; i < kSettingOffsets.size(); ++i) Write(manager + kSettingOffsets[i], savedDebugSettings[i]);
        settingsOwner = 0;
    }
}
void ForgetActor() {
    actor = 0;
    spawning = false;
    settingsOwner = 0;
    ds3scCompanionStatus.actor = 0;
    ds3scCompanionStatus.model = 0;
    ds3scCompanionStatus.drawEntity = 0;
    ds3sc::render::ActorTracker::Instance().SetActiveRenderingActor(0);
    ds3sc::render::ActorTracker::Instance().ResetCompanionCache();
}
void RemoveActor(Address world) {
    if (actor && InDebugList(world, actor) && actor != Read<Address>(world + 0x80)) {
        reinterpret_cast<void (*)(Address, Address)>(game + kRemove)(world, actor);
    }
    ForgetActor();
    ds3scCompanionStatus.state = 1;
}
void PrepareGoods() {
    if (rowReady.load()) return;
    Lookup existing{}, base{};
    auto lookup = reinterpret_cast<GoodsLookup>(bindings[0].original);
    lookup(owner, &existing, kGoods);
    if (existing.row) { ds3scCompanionStatus.error = 10; return; } // occupied ID
    lookup(owner, &base, 589000);
    if (!base.row || !Readable(reinterpret_cast<Address>(base.row), goodsRow.size())) return;
    std::memcpy(goodsRow.data(), base.row, goodsRow.size());
    std::memcpy(goodsRow.data(), &kAction, sizeof(kAction));
    rowReady.store(true, std::memory_order_release);
}
void EnsureGift() {
    if (!rowReady.load()) return;
    const auto wrapper = Read<Address>(Read<Address>(owner + 0x28) + 0x30);
    const auto data = Read<Address>(Read<Address>(wrapper));
    const auto playerData = Read<Address>(data + 0x10);
    const auto count = Read<Address>(wrapper + 0x30);
    const auto itemWrapper = Read<Address>(wrapper + 0x10);
    if (!playerData || !Readable(count, 16) || !Read<Address>(Read<Address>(itemWrapper))) return;
    if (reinterpret_cast<int (*)(Address, std::uint32_t, std::uint32_t)>(count)(playerData + 0x3d0, 0x40000000, kGoods) != 0) return;
    struct Item { std::uint32_t id, quantity, durability; } item{kGoods | 0x40000000, 1, 0};
    const std::array<const Item*, 3> vector{&item, &item + 1, &item + 1};
    reinterpret_cast<void (*)(Address, const void*, Address, Address)>(mod + 0x89370)(itemWrapper, &vector, 0, 0);
    ++ds3scCompanionStatus.giftCount;
}
struct Archetype {
    std::uint32_t model = 0;
    std::uint32_t npcParam = 0;
    std::uint32_t thinkParam = 0;
    Address sourceChr = 0;
};
Archetype FindMapArchetype(Address world) {
    Archetype best{};
    if (!world) return best;
    const auto groups = Read<std::uint32_t>(world + 0x1c8);
    if (groups > 64) return best;
    for (std::uint32_t g = 0; g < groups; ++g) {
        const auto header = Read<Address>(world + 0x1d0 + g * 0x18);
        if (!header) continue;
        const auto count = Read<std::uint32_t>(header);
        const auto entries = Read<Address>(header + 8);
        if (!entries || count > 512) continue;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto chr = Read<Address>(entries + i * 0x38);
            if (!chr) continue;
            const auto model = Read<Address>(chr + 0x48);
            if (!model) continue;
            const auto draw = Read<Address>(model + 0x8);
            if (!draw) continue;
            // SprjModelDrawEntity +0x20 is flags, not a mesh count.
            // +0x40/+0x50 are the world-space AABB min/max (three floats).
            // c1000 placeholders have zero extents despite non-null resources.
            bool hasGeometry = true;
            for (Address axis = 0; axis < 12; axis += 4) {
                const auto lo = Read<float>(draw + 0x40 + axis);
                const auto hi = Read<float>(draw + 0x50 + axis);
                const auto extent = hi - lo;
                if (!std::isfinite(lo) || !std::isfinite(hi) || extent < 0.02f || extent > 8.0f) hasGeometry = false;
            }
            if (!hasGeometry) continue;
            const auto res = Read<Address>(chr + 0x30);
            if (!res) continue;
            wchar_t midStr[5]{};
            for (int k = 0; k < 4; ++k) midStr[k] = Read<wchar_t>(res + 0x2a + k * 2);
            bool digits = true;
            for (auto digit : midStr) { if (digit && (digit < L'0' || digit > L'9')) digits = false; }
            if (!digits) continue;
            const auto modelId = static_cast<std::uint32_t>(
                (midStr[0] - L'0') * 1000 + (midStr[1] - L'0') * 100 +
                (midStr[2] - L'0') * 10 + (midStr[3] - L'0'));
            const auto npc = Read<std::uint32_t>(chr + 0x68);
            const auto think = Read<std::uint32_t>(chr + 0x6c);
            // Use complete scalar parameters from a real loaded non-player
            // archetype. Never synthesize IDs from a model number.
            if (modelId == 0 || modelId == 1000 || !npc || npc == ~0u || think == ~0u) continue;
            if (best.sourceChr == 0) best = {modelId, npc, think, chr};
            if (modelId == 1400) return {modelId, npc, think, chr};
        }
    }
    return best;
}
void BeginSpawn(Address world, Address manager) {
    if (!Solo(world) || Read<std::uint32_t>(manager + 0x48) != 0 || Read<std::uint32_t>(manager + 0x4c) != 0) return;
    // Reserve the native debug facility exclusively while this request is alive.
    if (Read<Address>(world + 0x2ff8) != Read<Address>(world + 0x3000)) { ds3scCompanionStatus.error = 11; return; }

    const auto archetype = FindMapArchetype(world);
    if (!archetype.sourceChr) { ds3scCompanionStatus.error = 14; return; }
    ds3scCompanionStatus.error = 0;

    // Preserve only scalar inputs, never copy the manager's owned strings,
    // resource vectors, allocator pointers or asynchronous loading state.
    for (std::size_t i = 0; i < kSettingOffsets.size(); ++i) savedDebugSettings[i] = Read<std::uint32_t>(manager + kSettingOffsets[i]);
    settingsOwner = manager;

    Write<std::uint32_t>(manager + 0x1b0, archetype.model);
    Write<std::uint32_t>(manager + 0x128, archetype.npcParam);
    Write<std::uint32_t>(manager + 0x124, archetype.thinkParam);
    Write<float>(manager + 0x1b4, 2.5f);
    Write<float>(manager + 0x1b8, 0.0f);
    Write<float>(manager + 0x1bc, 180.0f);
    spawning = true;
    started = GetTickCount64();
    ds3scCompanionStatus.state = 2;
    reinterpret_cast<void (*)(Address, int)>(game + kSpawn)(manager, 1);
}
void Tick(Address manager, Address dt) {
    originalUpdate(manager, dt);
    ++ds3scCompanionStatus.updates;
    const auto world = Read<Address>(game + kWorld);
    if (!world || manager != Read<Address>(world + 0x3018)) return;
    const auto local = Read<Address>(world + 0x80);
    if (world != activeWorld || local != activeLocal) {
        // A same-world load can replace the player without replacing the manager.
        if (world == activeWorld && actor) RemoveActor(world);
        if (spawning && settingsOwner == manager) {
            // Let the native request finish before retiring it below.
            enabled.store(false);
        } else {
            RestoreSettings(manager);
            ForgetActor();
        }
        activeWorld = world;
        activeLocal = local;
        lastGiftCheck = 0;
    }
    if (!local) return;
    const auto command = InterlockedExchange(&ds3scCompanionCommand, 0);
    if (command == 1) toggleRequested.store(true);
    if (command == 2) {
        lastGiftCheck = 0;
        PrepareGoods();
        EnsureGift();
    }
    if (command == 3) enabled.store(false);
    if (spawning && Read<std::uint32_t>(manager + 0x48) == 0 && Read<std::uint32_t>(manager + 0x4c) == 0) {
        const auto created = Read<Address>(manager + 0x1c8);
        if (InDebugList(world, created)) {
            actor = created;

            // Models and draw resources remain owned and initialized by DS3.
            // Do not copy opaque structures (including vtables/visibility state).
            // Friendly co-op white phantom team
            Write<std::uint32_t>(actor + 0x74, Read<std::uint32_t>(local + 0x74));

            // Character type: normal living character (0) matching host
            Write<std::uint32_t>(actor + 0x70, 0);

            // PhantomParamID: -1 = standard authentic visuals (NO white phantom glow)
            Write<std::int32_t>(actor + 0x1f38, -1);

            // No Attack (0x40) + No Hit (0x20) = 0x60
            // Do not set 0x80 (No Move) so idle animations update bones & bounds!
            Write<std::uint8_t>(actor + 0x1ee8, Read<std::uint8_t>(actor + 0x1ee8) | 0x60);

            ds3scCompanionStatus.actor = actor;
            const auto compModel = Read<Address>(actor + 0x48);
            ds3scCompanionStatus.model = compModel;
            ds3scCompanionStatus.drawEntity = compModel ? Read<Address>(compModel + 8) : 0;
            ds3scCompanionStatus.state = 3;
        } else { ds3scCompanionStatus.error = 12; ds3scCompanionStatus.state = 1; }
        spawning = false;
        RestoreSettings(manager);
    }
    if (spawning && GetTickCount64() - started > 15000) {
        // Do not restore inputs or free resources while the engine still loads.
        ds3scCompanionStatus.error = 13;
    }
    if (!enabled.load() || !Solo(world)) {
        if (actor) RemoveActor(world);
        toggleRequested.store(false);
        if (!enabled.load()) ds3scCompanionStatus.state = 4;
        return;
    }
    if (actor && !InDebugList(world, actor)) ForgetActor();
    if (actor) {
        if (Read<std::int32_t>(actor + 0x1f38) != -1) {
            Write<std::int32_t>(actor + 0x1f38, -1);
        }
    }
    const auto activeCompModel = actor ? Read<Address>(actor + 0x48) : 0;
    ds3scCompanionStatus.model = activeCompModel;
    const auto activeDrawEntity = activeCompModel ? Read<Address>(activeCompModel + 8) : 0;
    ds3scCompanionStatus.drawEntity = activeDrawEntity;

    const auto now = GetTickCount64();
    if (now - lastGiftCheck > 2000) {
        PrepareGoods();
        EnsureGift();
        lastGiftCheck = now;
    }
    if (toggleRequested.exchange(false) && !spawning && now - lastUse > 750) {
        lastUse = now;
        if (actor) RemoveActor(world);
        else BeginSpawn(world, manager);
    }
}

bool Discover() {
    owner = 0;
    for (auto& binding : bindings) {
        binding.slot = nullptr;
        binding.original = nullptr;
    }

    // Only scan the original mod's generated, private executable callback arena.
    MEMORY_BASIC_INFORMATION region{};
    for (Address address = 0; VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)); ) {
        const auto begin = reinterpret_cast<Address>(region.BaseAddress);
        const auto end = begin + region.RegionSize;
        if (end <= address) break;
        address = end;
        if (region.State != MEM_COMMIT || region.Type != MEM_PRIVATE || !(region.Protect & 0xf0) || region.Protect & PAGE_GUARD || region.RegionSize > 0x1000000) continue;
        for (auto p = begin; p + 28 <= end; ++p) {
            if (Read<std::uint16_t>(p) != 0xb848 || Read<std::uint32_t>(p + 10) != 0x6e0f4866 || Read<std::uint16_t>(p + 15) != 0xbb49 || Read<std::uint32_t>(p + 24) != 0xe3ff4100) {
                // The helper's high address byte is zero in a canonical user address.
                continue;
            }
            const auto closure = Read<Address>(p + 2), helper = Read<Address>(p + 17);
            if (helper < mod || helper >= mod + 0x1c0000) continue;
            const auto object = Read<Address>(closure + 8), target = Read<Address>(closure + 16);
            for (auto& binding : bindings) if (target == mod + binding.rva) {
                if (binding.slot && binding.slot != reinterpret_cast<void**>(closure + 16)) return false;
                if (owner && owner != object) return false;
                owner = object;
                binding.slot = reinterpret_cast<void**>(closure + 16);
                binding.original = reinterpret_cast<void*>(target);
            }
        }
    }
    for (const auto& binding : bindings) if (!binding.slot) return false;
    return owner != 0;
}

DWORD WINAPI Install(void*) {
    // Retry loop: wait for game and ds3sc.dll to be fully initialized
    for (int attempt = 0; attempt < 300; ++attempt) {
        game = reinterpret_cast<Address>(GetModuleHandleW(L"DarkSoulsIII.exe"));
        mod = reinterpret_cast<Address>(GetModuleHandleW(L"ds3sc.dll"));
        if (!game || !mod) {
            Sleep(1000);
            continue;
        }

        const auto pe = game + Read<std::uint32_t>(game + 0x3c);
        const std::array<std::uint8_t, 16> spawnPrefix{0x48,0x83,0xec,0x28,0xff,0xca,0x74,0x7e,0xff,0xca,0x74,0x3f,0xff,0xca,0x75,0x7f};
        if (Read<std::uint32_t>(pe + 8) != 0x639c4ddd || Read<std::uint32_t>(pe + 0x50) != 0x56f4000 ||
            Read<Address>(game + kDebugVtable + 0x48) != game + kDebugUpdate ||
            std::memcmp(reinterpret_cast<void*>(game + kSpawn), spawnPrefix.data(), spawnPrefix.size())) {
            Sleep(1000);
            continue;
        }

        if (!Discover()) {
            Sleep(1000);
            continue;
        }

        // Successfully discovered! Now install hooks
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Install), &pinned)) {
            ds3scCompanionStatus.error = 4;
            return 4;
        }

        // Instalar hooks D3D11 optimizados a 60 FPS (Present, ResizeBuffers, Draw calls y constantes ligeras)
        ds3sc::render::D3D11HookManager::Instance().Install();

        auto slot = reinterpret_cast<void**>(game + kDebugVtable + 0x48);
        DWORD previous{};
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
            ds3scCompanionStatus.error = 5;
            return 5;
        }
        originalUpdate = reinterpret_cast<Update>(*slot);
        for (auto& binding : bindings) InterlockedExchangePointer(binding.slot, binding.replacement);
        enabled.store(true);
        ds3scCompanionStatus.state = 1;
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(&Tick));
        DWORD ignored{};
        VirtualProtect(slot, sizeof(void*), previous, &ignored);
        ds3scCompanionStatus.error = 0;
        return 0;
    }

    ds3scCompanionStatus.error = 3;
    return 3;
}
}

extern "C" {
__declspec(dllexport) void ds3sc_toggle_ally_mask(int show) {
    ds3sc::render::AllyOutlineRenderer::Instance().Settings().showAllyMask = (show != 0);
}
__declspec(dllexport) void ds3sc_toggle_ally_outline(int enable) {
    ds3sc::render::AllyOutlineRenderer::Instance().Settings().showAllyOutline = (enable != 0);
}
__declspec(dllexport) void ds3sc_set_outline_thickness(float thickness) {
    ds3sc::render::AllyOutlineRenderer::Instance().Settings().thickness = thickness;
}
__declspec(dllexport) int ds3sc_is_d3d11_hooked() {
    return ds3sc::render::D3D11HookManager::Instance().IsInstalled() ? 1 : 0;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE worker = CreateThread(nullptr, 0, Install, nullptr, 0, nullptr)) CloseHandle(worker);

    }
    return TRUE;
}

