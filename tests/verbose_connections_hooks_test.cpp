#include "../src/extensions/verbose_connections/verbose_connections_extension.cpp"
#include <cassert>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    assert(argc == 2);
    // Map native code without running DllMain or resolving imports. This proves
    // MinHook can relocate the verified prologues without starting the game.
    auto module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    assert(module);
    using namespace ds3sc::extensions;
    VerboseConnectionsExtension extension;
    assert(extension.Initialize());
    assert(g_ready && g_showBanner);
    for (auto* hook : g_ownedHooks) assert(hook);
    assert(extension.Initialize()); // Reinitialization must not duplicate hooks.
    extension.Shutdown();
    assert(!g_ready);
    assert(Matches(module, kCreateLobbyRva, kCreateLobbyPrefix, sizeof(kCreateLobbyPrefix)));
    assert(Matches(module, kLeaveLobbyRva, kLeaveLobbyPrefix, sizeof(kLeaveLobbyPrefix)));
    assert(Matches(module, kSessionTickRva, kSessionTickPrefix, sizeof(kSessionTickPrefix)));
    FreeLibrary(module);
    std::puts("PASS: real MinHook installation, reinitialization and byte restoration on installed ds3sc.dll");
}
