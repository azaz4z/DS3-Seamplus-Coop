#include "companion_spawner_extension.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>

#if defined(DS3SC_FEATURE_ALLY_OUTLINE) && DS3SC_FEATURE_ALLY_OUTLINE
#include "../../render/actor_tracker.h"
inline void NotifyActorTrackerReset() {
    ds3sc::render::ActorTracker::Instance().SetActiveRenderingActor(0);
    ds3sc::render::ActorTracker::Instance().ResetCompanionCache();
}
#else
inline void NotifyActorTrackerReset() {}
#endif

// External export of status and command
extern "C" {
__declspec(dllexport) volatile CompanionStatus ds3scCompanionStatus{1, 0, 0, 0, 0, 0, 0, 0, 0};
__declspec(dllexport) volatile LONG ds3scCompanionCommand = 0;
}

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

void LookupGoods(Address self, Lookup* result, std::uint32_t id) {
    if (id == kGoods && rowReady.load(std::memory_order_acquire)) {
        *result = {kGoods, 2, goodsRow.data()};
        return;
    }
    reinterpret_cast<GoodsLookup>(bindings[0].original)(self, result, id);
}

static bool IsSpanishLanguage() noexcept {
    static int cached = -1;
    if (cached != -1) return cached == 1;

    char iniPath[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&IsSpanishLanguage), &hMod) && hMod) {
        GetModuleFileNameA(hMod, iniPath, sizeof(iniPath));
        char* lastSlash = std::strrchr(iniPath, '\\');
        if (lastSlash) {
            strcpy_s(lastSlash + 1, sizeof(iniPath) - (lastSlash + 1 - iniPath), "ds3sc_settings.ini");
        }
    }
    if (iniPath[0] == '\0') {
        if (GetFileAttributesA("SeamplusCoop\\ds3sc_settings.ini") != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(iniPath, "SeamplusCoop\\ds3sc_settings.ini");
        } else {
            strcpy_s(iniPath, "SeamlessCoop\\ds3sc_settings.ini");
        }
    }

    char langBuf[64] = {};
    GetPrivateProfileStringA("LANGUAGE", "mod_language_override", "", langBuf, sizeof(langBuf), iniPath);
    if (_stricmp(langBuf, "spanish") == 0 || _stricmp(langBuf, "es") == 0) {
        cached = 1;
        return true;
    }
    if (_stricmp(langBuf, "english") == 0 || _stricmp(langBuf, "en") == 0) {
        cached = 0;
        return false;
    }

    LANGID langId = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(langId) == LANG_SPANISH) {
        cached = 1;
        return true;
    }

    cached = 0;
    return false;
}

static bool ContainsCaseInsensitive(const wchar_t* haystack, const wchar_t* needle) noexcept {
    if (!haystack || !needle || !*needle) return false;
    const size_t hlen = wcslen(haystack);
    const size_t nlen = wcslen(needle);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; ++i) {
        if (_wcsnicmp(&haystack[i], needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

const wchar_t* LookupText(Address self, Address context, std::uint32_t language, int category, int id) {
    if (id == static_cast<int>(kGoods)) {
        if (category == 10) return L"Ash Stone (Summon Companion)";
        if (category == 20) return L"Summons or dismisses a local test ally NPC.";
        if (category == 24) return L"A magical ash stone for testing.\nSummons a single ally NPC near the bearer.\nUse again to dismiss. Solo only.";
    }
    const wchar_t* orig = reinterpret_cast<TextLookup>(bindings[1].original)(self, context, language, category, id);
    if (orig && orig[0] != L'\0') {
        __try {
            if (ContainsCaseInsensitive(orig, L"descargable") ||
                ContainsCaseInsensitive(orig, L"comprar contenido") ||
                ContainsCaseInsensitive(orig, L"purchase add-on") ||
                ContainsCaseInsensitive(orig, L"purchase downloadable") ||
                ContainsCaseInsensitive(orig, L"add-on content")) {
                if (wcslen(orig) <= 45) {
                    return L"Seamplus";
                } else {
                    return L"Configure Seamplus Co-op settings and features.";
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return orig;
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
    NotifyActorTrackerReset();
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
    if (existing.row) { ds3scCompanionStatus.error = 10; return; }
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
            if (modelId == 0 || modelId == 1000 || !npc || npc == ~0u || think == ~0u) continue;
            if (best.sourceChr == 0) best = {modelId, npc, think, chr};
            if (modelId == 1400) return {modelId, npc, think, chr};
        }
    }
    return best;
}

void BeginSpawn(Address world, Address manager) {
    if (!Solo(world) || Read<std::uint32_t>(manager + 0x48) != 0 || Read<std::uint32_t>(manager + 0x4c) != 0) return;
    if (Read<Address>(world + 0x2ff8) != Read<Address>(world + 0x3000)) { ds3scCompanionStatus.error = 11; return; }

    const auto archetype = FindMapArchetype(world);
    if (!archetype.sourceChr) { ds3scCompanionStatus.error = 14; return; }
    ds3scCompanionStatus.error = 0;

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
    if (originalUpdate) originalUpdate(manager, dt);
    ++ds3scCompanionStatus.updates;
    const auto world = Read<Address>(game + kWorld);
    if (!world || manager != Read<Address>(world + 0x3018)) return;
    const auto local = Read<Address>(world + 0x80);
    if (world != activeWorld || local != activeLocal) {
        if (world == activeWorld && actor) RemoveActor(world);
        if (spawning && settingsOwner == manager) {
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
            Write<std::uint32_t>(actor + 0x74, Read<std::uint32_t>(local + 0x74));
            Write<std::uint32_t>(actor + 0x70, 0);
            Write<std::int32_t>(actor + 0x1f38, -1);
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

    MEMORY_BASIC_INFORMATION region{};
    for (Address address = 0; VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)); ) {
        const auto begin = reinterpret_cast<Address>(region.BaseAddress);
        const auto end = begin + region.RegionSize;
        if (end <= address) break;
        address = end;
        if (region.State != MEM_COMMIT || region.Type != MEM_PRIVATE || !(region.Protect & 0xf0) || region.Protect & PAGE_GUARD || region.RegionSize > 0x1000000) continue;
        for (auto p = begin; p + 28 <= end; ++p) {
            if (Read<std::uint16_t>(p) != 0xb848 || Read<std::uint32_t>(p + 10) != 0x6e0f4866 || Read<std::uint16_t>(p + 15) != 0xbb49 || Read<std::uint32_t>(p + 24) != 0xe3ff4100) {
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
} // namespace

namespace ds3sc::extensions {

bool CompanionSpawnerExtension::Initialize() noexcept {
    game = reinterpret_cast<Address>(GetModuleHandleW(L"DarkSoulsIII.exe"));
    mod = reinterpret_cast<Address>(GetModuleHandleW(L"ds3sc.dll"));
    if (!game || !mod) {
        ds3scCompanionStatus.error = 1;
        return false;
    }

    const auto pe = game + Read<std::uint32_t>(game + 0x3c);
    const std::array<std::uint8_t, 16> spawnPrefix{0x48,0x83,0xec,0x28,0xff,0xca,0x74,0x7e,0xff,0xca,0x74,0x3f,0xff,0xca,0x75,0x7f};
    if (Read<std::uint32_t>(pe + 8) != 0x639c4ddd || Read<std::uint32_t>(pe + 0x50) != 0x56f4000 ||
        Read<Address>(game + kDebugVtable + 0x48) != game + kDebugUpdate ||
        std::memcmp(reinterpret_cast<void*>(game + kSpawn), spawnPrefix.data(), spawnPrefix.size())) {
        ds3scCompanionStatus.error = 2;
        return false;
    }

    if (!Discover()) {
        ds3scCompanionStatus.error = 3;
        return false;
    }

    auto slot = reinterpret_cast<void**>(game + kDebugVtable + 0x48);
    DWORD previous{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
        ds3scCompanionStatus.error = 5;
        return false;
    }
    originalUpdate = reinterpret_cast<Update>(*slot);
    for (auto& binding : bindings) InterlockedExchangePointer(binding.slot, binding.replacement);
    enabled.store(true);
    ds3scCompanionStatus.state = 1;
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&Tick));
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), previous, &ignored);
    ds3scCompanionStatus.error = 0;
    return true;
}

void CompanionSpawnerExtension::Shutdown() noexcept {
    enabled.store(false);
    if (game && originalUpdate) {
        auto slot = reinterpret_cast<void**>(game + kDebugVtable + 0x48);
        DWORD previous{};
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
            InterlockedExchangePointer(slot, reinterpret_cast<void*>(originalUpdate));
            DWORD ignored{};
            VirtualProtect(slot, sizeof(void*), previous, &ignored);
        }
    }
    for (auto& binding : bindings) {
        if (binding.slot && binding.original) {
            InterlockedExchangePointer(binding.slot, binding.original);
        }
    }
    ForgetActor();
    ds3scCompanionStatus.state = 4;
}

void CompanionSpawnerExtension::OnTick() noexcept {
    // The main tick runs hooked to native DS3 Update in &Tick
}

std::shared_ptr<IExtension> CreateCompanionSpawnerExtension() noexcept {
    return std::make_shared<CompanionSpawnerExtension>();
}

} // namespace ds3sc::extensions
