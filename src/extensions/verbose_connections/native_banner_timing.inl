// Included inside the extension's anonymous namespace. These functions run on
// the native menu thread; the existing game movie still renders every message.
constexpr size_t kBannerUpdateRva = 0xba1240;
constexpr size_t kBannerNextRva = 0xba16a0;
constexpr size_t kBannerTimingRva = 0x46117c8;
constexpr size_t kBannerTextRva = 0x46117e8;
constexpr size_t kBannerQueueRva = 0x478de10;
constexpr unsigned char kBannerUpdatePrefix[] = {
    0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x30,
    0x0f,0x29,0x74,0x24,0x20,0x48,0x8b,0xd9,0x0f,0x28,0xf1};
constexpr unsigned char kBannerNextPrefix[] = {
    0x48,0x8b,0xc4,0x57,0x48,0x81,0xec,0xd0,0x00,0x00,0x00,0x48,0xc7,0x44,0x24,
    0x28,0xfe,0xff,0xff,0xff};
// Verify the duration load AND the movie's timing/repetition inputs, not only
// generic function prologues, before interpreting the native data layout.
constexpr unsigned char kBannerDurationLoad[] = {
    0xf3,0x0f,0x10,0x05,0xeb,0x00,0xa7,0x03,0xf3,0x0f,0x11,0x81,0xf0,0x0a,0x00,0x00};
constexpr unsigned char kBannerCycleLoad[] = {
    0xf3,0x0f,0x10,0x35,0x54,0x04,0xa7,0x03,0x48,0x8d,0x0d,0x4d,0x04,0xa7,0x03};
constexpr unsigned char kBannerDefaults[] = {0,0,0xa0,0x40,3,0,0,0,60,0,0,0};
// R8 is the native update-context pointer. The base menu update at 0xa4ca90
// dereferences it; preserving only RCX/XMM1 crashes when a save is loaded.
using BannerUpdateFn = void(__fastcall*)(void*, float, void*);
using BannerNextFn = void(__fastcall*)(void*);
BannerUpdateFn g_bannerUpdate = nullptr;
BannerNextFn g_bannerNext = nullptr;
unsigned char* g_bannerGame = nullptr;
std::array<void*, 2> g_bannerHooks{};
struct BannerTiming { float duration; int cycles; };
thread_local const BannerTiming* g_bannerFrame = nullptr;

bool IsVerboseBanner(const void* nativeText) noexcept {
    // Native text is an optional direct pointer followed by an MSVC wide string
    // (inline capacity 7). Read only the six exact messages emitted here.
    __try {
        const auto* base = static_cast<const unsigned char*>(nativeText);
        if (!base) return false;
        auto* text = *reinterpret_cast<const wchar_t* const*>(base);
        if (!text) {
            base += 8;
            text = *reinterpret_cast<const size_t*>(base + 0x18) >= 8
                ? *reinterpret_cast<const wchar_t* const*>(base)
                : reinterpret_cast<const wchar_t*>(base);
        }
        const wchar_t* const messages[] = {
            L"Creando sesión vía LAN...", L"Creating session via LAN...",
            L"Creando sesión vía Steam...", L"Creating session via Steam...",
            L"Has salido de la sesión cooperativa", L"You have left the co-op session"};
        for (const auto* message : messages) {
            size_t i = 0;
            while (message[i] && message[i] == text[i]) ++i;
            if (!message[i] && !text[i]) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SelectBannerTiming(bool verbose) noexcept {
    if (!g_bannerFrame) return;
    const BannerTiming timing = verbose ? BannerTiming{2.0f, 1} : *g_bannerFrame;
    std::memcpy(g_bannerGame + kBannerTimingRva, &timing, sizeof(timing));
}

void __fastcall DetourBannerNext(void* menu) {
    // A different notification can replace the current one during Update.
    // Select its settings BEFORE the native dequeue initializes the timer.
    auto* head = Pointer(g_bannerGame, kBannerQueueRva);
    auto* next = static_cast<unsigned char*>(Pointer(head, 0));
    SelectBannerTiming(next && IsVerboseBanner(next + 0x18));
    g_bannerNext(menu);
}

void __fastcall DetourBannerUpdate(void* menu, float delta, void* updateContext) {
    if (!g_ready.load(std::memory_order_acquire)) { g_bannerUpdate(menu, delta, updateContext); return; }
    BannerTiming saved{};
    std::memcpy(&saved, g_bannerGame + kBannerTimingRva, sizeof(saved));
    const auto* previous = g_bannerFrame;
    g_bannerFrame = &saved;
    SelectBannerTiming(IsVerboseBanner(g_bannerGame + kBannerTextRva));
    __try {
        g_bannerUpdate(menu, delta, updateContext);
    } __finally {
        // Never leave a global duration/repetition change behind for other UI.
        std::memcpy(g_bannerGame + kBannerTimingRva, &saved, sizeof(saved));
        g_bannerFrame = previous;
    }
}

void RemoveBannerTimingHooks() noexcept {
    for (auto& target : g_bannerHooks) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }
}

bool InstallBannerTimingHooks(void* module) noexcept {
    if (!Matches(module, kBannerUpdateRva, kBannerUpdatePrefix, sizeof(kBannerUpdatePrefix)) ||
        !Matches(module, kBannerNextRva, kBannerNextPrefix, sizeof(kBannerNextPrefix)) ||
        !Matches(module, 0xba16d5, kBannerDurationLoad, sizeof(kBannerDurationLoad)) ||
        !Matches(module, 0xba136c, kBannerCycleLoad, sizeof(kBannerCycleLoad)) ||
        !Matches(module, kBannerTimingRva, kBannerDefaults, sizeof(kBannerDefaults))) {
        OutputDebugStringA("[ds3sc-verbose] Unsupported native banner timing layout.\n");
        return false;
    }
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    auto* base = static_cast<unsigned char*>(module);
    const std::array<void*, 2> targets{base + kBannerUpdateRva, base + kBannerNextRva};
    const std::array<void*, 2> detours{reinterpret_cast<void*>(&DetourBannerUpdate), reinterpret_cast<void*>(&DetourBannerNext)};
    const std::array<void**, 2> originals{reinterpret_cast<void**>(&g_bannerUpdate), reinterpret_cast<void**>(&g_bannerNext)};
    for (size_t i = 0; i < targets.size(); ++i) {
        if (MH_CreateHook(targets[i], detours[i], originals[i]) != MH_OK) {
            RemoveBannerTimingHooks();
            return false;
        }
        g_bannerHooks[i] = targets[i];
    }
    g_bannerGame = base;
    for (auto* target : targets) {
        if (MH_EnableHook(target) != MH_OK) {
            RemoveBannerTimingHooks();
            return false;
        }
    }
    return true;
}
