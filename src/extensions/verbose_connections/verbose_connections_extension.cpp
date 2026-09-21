#include "verbose_connections_extension.h"
#include "../../../tools/vendor/minhook-1.3.4/include/MinHook.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace ds3sc::extensions {
namespace {
using ShowBannerFn = void(__fastcall*)(void*, const wchar_t*);
using LobbyActionFn = uint64_t(__fastcall*)(void*);
using SessionTickFn = void(__fastcall*)(void*);
ShowBannerFn g_showBanner = nullptr;
LobbyActionFn g_createLobby = nullptr;
LobbyActionFn g_leaveLobby = nullptr;
SessionTickFn g_sessionTick = nullptr;
std::array<void*, 3> g_ownedHooks{};
std::atomic<VerboseConnectionsExtension*> g_instance{nullptr};
std::atomic<bool> g_ready{false};
std::atomic<uint32_t> g_displayed{0}, g_failed{0};
struct PendingMessage { const wchar_t* text; uint64_t expires; };
std::mutex g_queueMutex;
std::deque<PendingMessage> g_messages;

template<class T>
bool Read(const void* base, size_t offset, T& value) noexcept {
    if (!base) return false;
    __try {
        std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* Pointer(const void* base, size_t offset) noexcept {
    void* value = nullptr;
    Read(base, offset, value);
    return value;
}

bool Show(void* manager, const wchar_t* text) noexcept {
    if (!g_showBanner || !manager) return false;
    __try {
        g_showBanner(manager, text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Queue(const wchar_t* text) {
    std::lock_guard lock(g_queueMutex);
    if (g_messages.size() == 8) {
        g_messages.pop_front();
        ++g_failed;
    }
    g_messages.push_back({text, GetTickCount64() + 15000});
    OutputDebugStringW(text);
}

// Called exclusively from native game-thread detours. Never call the game's
// menu functions from ExtensionWorker, and never retain a stale manager pointer.
void Flush(void* bannerManager) {
    if (!g_ready.load(std::memory_order_acquire)) return;
    PendingMessage message{};
    {
        std::lock_guard lock(g_queueMutex);
        const auto now = GetTickCount64();
        while (!g_messages.empty() && g_messages.front().expires < now) {
            g_messages.pop_front();
            ++g_failed;
            OutputDebugStringA("[ds3sc-verbose] Banner expired without delivery.\n");
        }
        if (g_messages.empty() || !bannerManager) return;
        message = g_messages.front();
        g_messages.pop_front();
    }
    if (Show(bannerManager, message.text)) {
        ++g_displayed;
    } else {
        std::lock_guard lock(g_queueMutex);
        g_messages.push_front(message); // Preserve the message for a later game tick.
        OutputDebugStringA("[ds3sc-verbose] Banner unavailable; delivery will retry.\n");
    }
}

// Verified in ds3sc 0.1.2: state objects use +0x20 for the menu wrapper,
// and the shared session context used by 0x8d9a0 uses +0x60.
uint64_t __fastcall DetourCreateLobby(void* state) {
    if (g_ready.load(std::memory_order_acquire)) {
        if (auto* instance = g_instance.load()) instance->TriggerHostingNotification();
        Flush(Pointer(state, 0x20));
    }
    return g_createLobby(state);
}

uint64_t __fastcall DetourLeaveLobby(void* state) {
    uint64_t lobby = 0;
    uint64_t alternateSession = 0;
    auto* data = Pointer(state, 0x48);
    Read(data, 0x118, lobby);
    Read(data, 0x130, alternateSession);
    const auto result = g_leaveLobby(state);
    // The native function also runs during empty-session cleanup. Only announce
    // a real LeaveLobby, whose argument was the nonzero lobby ID at +0x118.
    if (lobby && !alternateSession && g_ready.load(std::memory_order_acquire)) {
        if (auto* instance = g_instance.load()) instance->TriggerLeavingNotification();
        Flush(Pointer(state, 0x20));
    }
    return result;
}

void __fastcall DetourSessionTick(void* session) {
    g_sessionTick(session);
    Flush(Pointer(session, 0x60));
}

// No broad prologue scan: these offsets AND signatures describe one verified
// native ABI. A similar-looking function is not a safe fallback.
constexpr size_t kShowBannerRva = 0x895c0;
constexpr unsigned char kShowBannerPrefix[] = {
    0x56,0x57,0x53,0x48,0x83,0xec,0x60,0x48,0x89,0xd3,0x48,0x89,0xce};
constexpr size_t kCreateLobbyRva = 0x91150;
constexpr unsigned char kCreateLobbyPrefix[] = {
    0xf3,0x0f,0x1e,0xfa,0x56,0x57,0x53,0x48,0x83,0xec,0x20,
    0x48,0x89,0xce,0x48,0x8b,0x59,0x48,0x48,0x8d,0x7b,0x68,0x31,0xc9};
constexpr size_t kLeaveLobbyRva = 0x949c0;
constexpr unsigned char kLeaveLobbyPrefix[] = {
    0xf3,0x0f,0x1e,0xfa,0x48,0x83,0xec,0x28,0x48,0x8b,0x41,0x48,
    0x48,0x83,0xb8,0x30,0x01,0x00,0x00,0x00,0x74,0x18};
constexpr size_t kSessionTickRva = 0x8d9a0;
constexpr unsigned char kSessionTickPrefix[] = {
    0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,
    0x48,0x81,0xec,0x68,0x01,0x00,0x00};

bool Matches(const void* module, size_t rva, const unsigned char* prefix, size_t length) noexcept {
    __try {
        const auto* base = static_cast<const unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            rva >= nt->OptionalHeader.SizeOfImage || length > nt->OptionalHeader.SizeOfImage - rva) return false;
        return std::memcmp(base + rva, prefix, length) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void RemoveNativeHooks() noexcept {
    g_ready.store(false, std::memory_order_release);
    for (auto& target : g_ownedHooks) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }
    g_showBanner = nullptr;
}

bool InstallNativeHooks(void* module) noexcept {
    if (!Matches(module, kShowBannerRva, kShowBannerPrefix, sizeof(kShowBannerPrefix)) ||
        !Matches(module, kCreateLobbyRva, kCreateLobbyPrefix, sizeof(kCreateLobbyPrefix)) ||
        !Matches(module, kLeaveLobbyRva, kLeaveLobbyPrefix, sizeof(kLeaveLobbyPrefix)) ||
        !Matches(module, kSessionTickRva, kSessionTickPrefix, sizeof(kSessionTickPrefix))) {
        OutputDebugStringA("[ds3sc-verbose] Unsupported ds3sc.dll: lobby/banner signatures do not match.\n");
        return false;
    }
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    auto* base = static_cast<unsigned char*>(module);
    const std::array<void*, 3> targets{base + kCreateLobbyRva, base + kLeaveLobbyRva, base + kSessionTickRva};
    const std::array<void*, 3> detours{reinterpret_cast<void*>(&DetourCreateLobby),
        reinterpret_cast<void*>(&DetourLeaveLobby), reinterpret_cast<void*>(&DetourSessionTick)};
    const std::array<void**, 3> originals{reinterpret_cast<void**>(&g_createLobby),
        reinterpret_cast<void**>(&g_leaveLobby), reinterpret_cast<void**>(&g_sessionTick)};
    for (size_t i = 0; i < targets.size(); ++i) {
        auto result = MH_CreateHook(targets[i], detours[i], originals[i]);
        if (result == MH_OK) {
            g_ownedHooks[i] = targets[i];
        } else {
            OutputDebugStringA("[ds3sc-verbose] Could not create lobby hook.\n");
            RemoveNativeHooks();
            return false;
        }
    }
    g_showBanner = reinterpret_cast<ShowBannerFn>(base + kShowBannerRva);
    for (auto* target : targets) {
        if (MH_EnableHook(target) != MH_OK) {
            OutputDebugStringA("[ds3sc-verbose] Could not enable lobby hook.\n");
            RemoveNativeHooks();
            return false;
        }
    }
    return true;
}
} // namespace

VerboseConnectionsExtension::VerboseConnectionsExtension() noexcept { g_instance.store(this); }
VerboseConnectionsExtension::~VerboseConnectionsExtension() {
    auto* expected = this;
    g_instance.compare_exchange_strong(expected, nullptr);
}

void VerboseConnectionsExtension::LoadSettings() noexcept {
    HMODULE module = GetModuleHandleW(L"ds3sc.dll");
    if (!module) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Show), &module);
    iniPath_[0] = '\0';
    if (GetModuleFileNameA(module, iniPath_, MAX_PATH) >= MAX_PATH) { iniPath_[0] = '\0'; return; }
    if (auto* slash = std::strrchr(iniPath_, '\\')) {
        strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - iniPath_), "ds3sc_settings.ini");
    } else { iniPath_[0] = '\0'; }
}

bool VerboseConnectionsExtension::IsSpanish() const noexcept {
    char language[64]{};
    GetPrivateProfileStringA("LANGUAGE", "mod_language_override", "", language, sizeof(language), iniPath_);
    if (!_stricmp(language, "spanish") || !_stricmp(language, "es")) return true;
    if (!_stricmp(language, "english") || !_stricmp(language, "en")) return false;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
}

ConnectionType VerboseConnectionsExtension::GetCurrentConnectionType() const noexcept {
    HMODULE companion = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&Show), &companion);
    using IsLanActiveFn = int(*)();
    auto isLan = companion ? reinterpret_cast<IsLanActiveFn>(GetProcAddress(companion, "ds3sc_is_lan_coop_active")) : nullptr;
    if (isLan) return isLan() ? ConnectionType::LAN : ConnectionType::Steam;
    return GetPrivateProfileIntA("NETWORK", "connection_mode", 0, iniPath_) == 1 ? ConnectionType::LAN : ConnectionType::Steam;
}

void VerboseConnectionsExtension::TriggerHostingNotification() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return;
    ++hostNotifications_;
    const bool spanish = IsSpanish();
    Queue(GetCurrentConnectionType() == ConnectionType::LAN
        ? (spanish ? L"Creando sesión vía LAN..." : L"Creating session via LAN...")
        : (spanish ? L"Creando sesión vía Steam..." : L"Creating session via Steam..."));
}

void VerboseConnectionsExtension::TriggerLeavingNotification() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) return;
    Queue(IsSpanish() ? L"Has salido de la sesión cooperativa" : L"You have left the co-op session");
}

void VerboseConnectionsExtension::GetStats(uint32_t& host, uint32_t& join) const noexcept {
    host = hostNotifications_.load();
    join = 0; // Preserve the original export ABI; this is not a delivery count.
}
bool VerboseConnectionsExtension::InstallHooks() noexcept { return InstallNativeHooks(GetModuleHandleW(L"ds3sc.dll")); }
void VerboseConnectionsExtension::RemoveHooks() noexcept { RemoveNativeHooks(); }
bool VerboseConnectionsExtension::Initialize() noexcept {
    if (initialized_.load()) return true;
    LoadSettings();
    if (!InstallHooks()) return false;
    initialized_.store(true, std::memory_order_release);
    g_ready.store(true, std::memory_order_release);
    OutputDebugStringA("[ds3sc-verbose] Lobby creation, leave and banner delivery hooks ready.\n");
    return true;
}
void VerboseConnectionsExtension::Shutdown() noexcept {
    initialized_.store(false, std::memory_order_release);
    RemoveHooks();
    std::lock_guard lock(g_queueMutex);
    g_messages.clear();
}
void VerboseConnectionsExtension::OnTick() noexcept {
    // ExtensionWorker is not the game's menu thread. Delivery happens in detours.
}
std::shared_ptr<VerboseConnectionsExtension> CreateVerboseConnectionsExtension() noexcept {
    return std::make_shared<VerboseConnectionsExtension>();
}
VerboseConnectionsExtension* GetVerboseConnectionsInstance() noexcept { return g_instance.load(); }
} // namespace ds3sc::extensions

extern "C" {
void ds3sc_trigger_verbose_host_notification() {
    if (auto* instance = ds3sc::extensions::GetVerboseConnectionsInstance()) instance->TriggerHostingNotification();
}
void ds3sc_trigger_verbose_leave_notification() {
    if (auto* instance = ds3sc::extensions::GetVerboseConnectionsInstance()) instance->TriggerLeavingNotification();
}
void ds3sc_get_verbose_connection_stats(uint32_t* host, uint32_t* join) {
    uint32_t h = 0, j = 0;
    if (auto* instance = ds3sc::extensions::GetVerboseConnectionsInstance()) instance->GetStats(h, j);
    if (host) *host = h;
    if (join) *join = j;
}
void ds3sc_get_verbose_delivery_stats(uint32_t* displayed, uint32_t* failed) {
    if (displayed) *displayed = ds3sc::extensions::g_displayed.load();
    if (failed) *failed = ds3sc::extensions::g_failed.load();
}
}
