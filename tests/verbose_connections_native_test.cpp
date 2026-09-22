#include "../src/extensions/extension.h"
#include <atomic>
#include <memory>
// Inspect lifecycle state without exposing test controls in the shipped DLL.
#define private public
#include "../src/extensions/verbose_connections/verbose_connections_extension.h"
#undef private
#include "../src/extensions/verbose_connections/verbose_connections_extension.cpp"
#include <cassert>
#include <string>
#include <vector>
#include <set>
#include <cmath>

using namespace ds3sc::extensions;
namespace {
int createCalls = 0, enableCalls = 0, failCreate = 0, failEnable = 0;
std::set<void*> owned;
std::vector<void*> removed;
std::vector<std::wstring> banners;
bool rejectBanner = false;
int nativeCreates = 0, nativeLeaves = 0, nativeTicks = 0;
void* expectedBanner = reinterpret_cast<void*>(0x123400);
void __fastcall Banner(void* manager, const wchar_t* text) {
    assert(manager == expectedBanner);
    if (rejectBanner) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    banners.emplace_back(text);
}
uint64_t __fastcall Create(void*) { ++nativeCreates; return 0x123456789abcdef0; }
uint64_t __fastcall Leave(void* state) {
    ++nativeLeaves;
    auto* data = static_cast<unsigned char*>(Pointer(state, 0x48));
    std::memset(data + 0x118, 0, 8);
    return 0x1020304050607080;
}
void __fastcall Tick(void*) { ++nativeTicks; }
template<class T> void Write(void* base, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(base) + offset, &value, sizeof(value));
}
const wchar_t* nextText = nullptr;
float nativeRemaining = 0;
int nativeFrame = 0;
bool throwInBanner = false;
unsigned char nativeUpdateContext = 1;
void* expectedMenu = reinterpret_cast<void*>(0x12345670);
float expectedDelta = 0;
int bannerUpdates = 0;
BannerTiming ObservedTiming() {
    BannerTiming timing{};
    std::memcpy(&timing, g_bannerGame + kBannerTimingRva, sizeof(timing));
    return timing;
}
void __fastcall NativeBannerNext(void*) {
    Write(g_bannerGame, kBannerTextRva, nextText);
    nativeRemaining = ObservedTiming().duration;
    nextText = nullptr;
}
void __fastcall NativeBannerUpdate(void* menu, float delta, void* updateContext) {
    assert(menu == expectedMenu && delta == expectedDelta);
    assert(updateContext == &nativeUpdateContext);
    assert(*static_cast<unsigned char*>(updateContext) == nativeUpdateContext);
    ++bannerUpdates;
    if (throwInBanner) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    nativeRemaining -= delta;
    if (nextText) DetourBannerNext(menu);
    const auto timing = ObservedTiming();
    // Same phase calculation as DS3's native movie update at 0xba136c.
    const float segment = timing.duration / timing.cycles;
    nativeFrame = nativeRemaining > 0
        ? 1 + static_cast<int>(59 * std::fmod(timing.duration - nativeRemaining, segment) / segment) : 0;
}
void ProtectedBannerUpdate() {
    __try { DetourBannerUpdate(expectedMenu, 0, &nativeUpdateContext); assert(false); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void TestNativeBannerTiming() {
    std::vector<unsigned char> game(0x4790000);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(game.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(game.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(game.size());
    std::memcpy(game.data()+kBannerUpdateRva,kBannerUpdatePrefix,sizeof(kBannerUpdatePrefix));
    std::memcpy(game.data()+kBannerNextRva,kBannerNextPrefix,sizeof(kBannerNextPrefix));
    std::memcpy(game.data()+0xba16d5,kBannerDurationLoad,sizeof(kBannerDurationLoad));
    std::memcpy(game.data()+0xba136c,kBannerCycleLoad,sizeof(kBannerCycleLoad));
    std::memcpy(game.data()+kBannerTimingRva,kBannerDefaults,sizeof(kBannerDefaults));
    createCalls = enableCalls = failCreate = failEnable = 0;
    assert(!InstallBannerTimingHooks(nullptr));
    game[0xba16d5] ^= 1;
    assert(!InstallBannerTimingHooks(game.data()) && createCalls == 0);
    game[0xba16d5] ^= 1;
    failCreate = 2;
    assert(!InstallBannerTimingHooks(game.data()) && owned.empty());
    createCalls = 0; failCreate = 0; failEnable = 2;
    assert(!InstallBannerTimingHooks(game.data()) && owned.empty());
    enableCalls = 0; failEnable = 0;
    assert(InstallBannerTimingHooks(game.data()) && owned.size() == 2);
    g_bannerUpdate = &NativeBannerUpdate;
    g_bannerNext = &NativeBannerNext;
    g_ready = true;
    auto update = [&](float delta) {
        expectedDelta = delta;
        const int before = bannerUpdates;
        DetourBannerUpdate(expectedMenu, delta, &nativeUpdateContext);
        assert(bannerUpdates == before + 1);
    };
    std::array<unsigned char, 0x60> node{};
    void* first = node.data();
    Write(game.data(), kBannerQueueRva, &first);
    auto enqueue = [&](const wchar_t* text) {
        nextText = text;
        Write(node.data(), 0x18, text);
    };
    auto checkRestored = [&]() {
        assert(std::memcmp(game.data()+kBannerTimingRva,kBannerDefaults,sizeof(kBannerDefaults)) == 0);
        assert(g_bannerFrame == nullptr);
    };
    for (const int fps : {30, 60, 144, 240}) {
        Write(game.data(), kBannerTextRva, L"Ordinary game notification");
        enqueue(L"Creando sesión vía Steam...");
        update(0);
        assert(nativeRemaining == 2 && nativeFrame == 1);
        checkRestored();
        int previous = nativeFrame;
        for (int i = 1; i < 2 * fps; ++i) {
            update(1.0f / fps);
            assert(nativeFrame >= previous && nativeFrame < 60); // No animation restart/blink.
            previous = nativeFrame;
            checkRestored();
        }
        update(1.0f / fps + 0.001f);
        assert(nativeFrame == 0 && nativeRemaining <= 0); // Two seconds, within one frame.
    }
    // Switching to a normal message restores its five seconds and three cycles
    // immediately, even when it replaces our banner inside the same update.
    enqueue(L"You have left the co-op session");
    update(0);
    assert(nativeRemaining == 2);
    enqueue(L"Ordinary game notification");
    update(0);
    assert(nativeRemaining == 5);
    checkRestored();
    update(1.0f);
    const auto beforeWrap = nativeFrame;
    update(0.7f);
    assert(nativeFrame < beforeWrap); // Ordinary game's three-cycle behavior retained.
    checkRestored();
    // Both the direct-string and owned native wstring layouts are recognized.
    std::array<unsigned char, 0x30> text{};
    Write(text.data(), 8, L"Has salido de la sesión cooperativa");
    Write(text.data(), 0x20, size_t{40});
    assert(IsVerboseBanner(text.data()));
    Write(text.data(), 8, L"Unrelated notification");
    assert(!IsVerboseBanner(text.data()));
    assert(!IsVerboseBanner(nullptr));
    Write(game.data(), kBannerTextRva, L"Creating session via LAN...");
    throwInBanner = true;
    expectedDelta = 0;
    ProtectedBannerUpdate();
    throwInBanner = false;
    checkRestored();
    g_ready = false;
    update(0.125f); // Disabled/initialization path must also preserve all arguments.
    RemoveBannerTimingHooks();
    assert(owned.empty());
    std::puts("PASS: native two-second single-cycle timing at 30/60/144/240 FPS, message replacement, restoration and rollback");
}
}
extern "C" {
MH_STATUS WINAPI MH_Initialize() { return MH_ERROR_ALREADY_INITIALIZED; }
MH_STATUS WINAPI MH_CreateHook(void* target, void*, void** original) {
    if (++createCalls == failCreate) return MH_ERROR_UNSUPPORTED_FUNCTION;
    assert(target && owned.insert(target).second);
    *original = target;
    return MH_OK;
}
MH_STATUS WINAPI MH_EnableHook(void* target) {
    assert(target && owned.count(target));
    return ++enableCalls == failEnable ? MH_ERROR_MEMORY_PROTECT : MH_OK;
}
MH_STATUS WINAPI MH_DisableHook(void* target) {
    assert(target && owned.count(target)); // Never MH_ALL_HOOKS.
    return MH_OK;
}
MH_STATUS WINAPI MH_RemoveHook(void* target) {
    assert(target && owned.erase(target) == 1);
    removed.push_back(target);
    return MH_OK;
}
}

int main() {
    std::vector<unsigned char> image(0xa0000);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
    std::memcpy(image.data()+kShowBannerRva,kShowBannerPrefix,sizeof(kShowBannerPrefix));
    std::memcpy(image.data()+kCreateLobbyRva,kCreateLobbyPrefix,sizeof(kCreateLobbyPrefix));
    std::memcpy(image.data()+kLeaveLobbyRva,kLeaveLobbyPrefix,sizeof(kLeaveLobbyPrefix));
    std::memcpy(image.data()+kSessionTickRva,kSessionTickPrefix,sizeof(kSessionTickPrefix));
    assert(!InstallNativeHooks(nullptr));
    image[kCreateLobbyRva] ^= 1;
    assert(!InstallNativeHooks(image.data()) && createCalls == 0);
    image[kCreateLobbyRva] ^= 1;
    failCreate = 2;
    assert(!InstallNativeHooks(image.data()) && owned.empty() && removed.size() == 1);
    createCalls = 0; failCreate = 0; failEnable = 2;
    assert(!InstallNativeHooks(image.data()) && owned.empty() && !g_ready);
    enableCalls = 0; failEnable = 0;
    assert(InstallNativeHooks(image.data()) && owned.size() == 3);
    RemoveNativeHooks();
    assert(owned.empty());

    VerboseConnectionsExtension extension;
    extension.LoadSettings();
    assert(std::strstr(extension.iniPath_, "verbose-tests"));
    WritePrivateProfileStringA("LANGUAGE", "mod_language_override", "spanish", extension.iniPath_);
    WritePrivateProfileStringA("NETWORK", "connection_mode", "0", extension.iniPath_);
    extension.initialized_.store(true);
    g_ready = true;
    g_showBanner = Banner; g_createLobby = Create; g_leaveLobby = Leave; g_sessionTick = Tick;
    std::array<unsigned char, 0x80> state{};
    std::array<unsigned char, 0x140> data{};
    std::array<unsigned char, 0x80> session{};
    Write(state.data(), 0x20, expectedBanner);
    Write(state.data(), 0x48, data.data());
    Write(session.data(), 0x60, expectedBanner);
    assert(DetourCreateLobby(state.data()) == 0x123456789abcdef0);
    assert(nativeCreates == 1 && banners.size() == 1);
    assert(banners.back() == L"Creando sesión vía Steam...");
    Write(data.data(), 0x118, uint64_t{123});
    assert(DetourLeaveLobby(state.data()) == 0x1020304050607080);
    assert(banners.back() == L"Has salido de la sesión cooperativa");
    DetourLeaveLobby(state.data());
    assert(banners.size() == 2); // Empty cleanup must be silent.
    // Rapid create/leave/create must not be suppressed by a shared debounce.
    WritePrivateProfileStringA("NETWORK", "connection_mode", "1", extension.iniPath_);
    DetourCreateLobby(state.data());
    assert(banners.back() == L"Creando sesión vía LAN...");
    WritePrivateProfileStringA("LANGUAGE", "mod_language_override", "english", extension.iniPath_);
    DetourCreateLobby(state.data());
    assert(banners.back() == L"Creating session via LAN...");
    const auto shown = banners.size();
    rejectBanner = true;
    DetourCreateLobby(state.data());
    assert(banners.size() == shown && g_messages.size() == 1);
    extension.OnTick();
    assert(g_messages.size() == 1); // Worker cannot call the native menu.
    rejectBanner = false;
    DetourSessionTick(session.data());
    assert(nativeTicks == 1 && banners.size() == shown+1 && g_messages.empty());
    Write(state.data(), 0x20, static_cast<void*>(nullptr));
    DetourCreateLobby(state.data());
    assert(g_messages.size() == 1);
    DetourSessionTick(session.data());
    assert(g_messages.empty());
    Queue(L"expired"); g_messages.front().expires = 0;
    DetourSessionTick(session.data());
    assert(g_failed == 1 && g_messages.empty());
    uint32_t hosts = 0;
    ds3sc_get_verbose_connection_stats(&hosts, nullptr);
    assert(hosts == 5);
    ds3sc_get_verbose_connection_stats(nullptr, nullptr);
    ds3sc_get_verbose_delivery_stats(nullptr, nullptr);
    extension.Shutdown();
    extension.TriggerHostingNotification();
    assert(g_messages.empty() && !g_ready);
    std::puts("PASS: native lobby events, ABI returns, language/mode, retries, cleanup and hook rollback");
    TestNativeBannerTiming();
}
