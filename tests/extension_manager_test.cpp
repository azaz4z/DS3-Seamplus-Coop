#include "../src/extensions/extension_manager.h"
#include <cstdio>

using namespace ds3sc::extensions;
struct Extension : IExtension {
    const char* id;
    int attempts = 0, shutdowns = 0, ticks = 0;
    bool ready;
    Extension(const char* name, bool succeeds) : id(name), ready(succeeds) {}
    const char* GetId() const noexcept override { return id; }
    const char* GetName() const noexcept override { return id; }
    bool Initialize() noexcept override { ++attempts; return ready; }
    void Shutdown() noexcept override { ++shutdowns; }
    void OnTick() noexcept override { ++ticks; }
};
int main() {
    auto& manager = ExtensionManager::Instance();
    auto outline = std::make_shared<Extension>("outline", false);
    auto companion = std::make_shared<Extension>("companion", true);
    manager.Register(outline); manager.Register(companion);
    if (manager.InitializeAll() || manager.IsActive("outline") || !manager.IsActive("companion")) return 1;
    manager.OnTick();
    if (outline->ticks != 0 || companion->ticks != 1) return 2;
    outline->ready = true;
    if (!manager.InitializeAll() || !manager.IsActive("outline")) return 3;
    if (companion->attempts != 1 || outline->attempts != 2) return 4;
    manager.ShutdownAll();
    if (outline->shutdowns != 1 || companion->shutdowns != 1 || manager.IsActive("outline")) return 5;
    std::puts("PASS: failed extension is retried, successful hooks are not initialized twice, active lifecycle is truthful.");
}
