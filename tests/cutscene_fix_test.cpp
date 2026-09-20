#include "../src/extensions/cutscene_fix/cutscene_fix_extension.h"
#include "../src/render/ally_pass_visibility.h"
#include "../src/extensions/extension_manager.h"

#include <cassert>
#include <cstdio>
#include <vector>
#include <string>

using namespace ds3sc::extensions;
using namespace ds3sc::render;

void TestCutsceneFixLifecycleAndStats() {
    auto ext = CreateCutsceneFixExtension();
    assert(ext != nullptr);
    assert(std::string(ext->GetId()) == "cutscene_fix");
    assert(ext->GetName() != nullptr);

    auto cutsceneFix = std::dynamic_pointer_cast<CutsceneFixExtension>(ext);
    assert(cutsceneFix != nullptr);

    assert(cutsceneFix->GetCutscenesHandled() == 0);
    assert(!cutsceneFix->IsInCutscene());

    assert(cutsceneFix->Initialize());
    cutsceneFix->OnTick();
    cutsceneFix->Shutdown();

    std::puts("PASS: TestCutsceneFixLifecycleAndStats completed.");
}

void TestCutsceneVisibilityOverride() {
    // Simulate a DrawEntity buffer with size > 0xc40
    std::vector<std::uint8_t> mockEntity(0x1000, 0);
    const auto entityPtr = reinterpret_cast<std::uintptr_t>(mockEntity.data());

    // Initially make it visible
    mockEntity[0xc3c] = 0x01;
    mockEntity[0xbb0] = 0x01;
    assert((mockEntity[0xc3c] & 1) == 1);
    assert((mockEntity[0xbb0] & 1) == 1);

    // Hide ally from cutscene
    HideAllyFromCutscene(entityPtr);
    assert((mockEntity[0xc3c] & 1) == 0);
    assert((mockEntity[0xbb0] & 1) == 0);

    // Make ally visible again
    mockEntity[0xc3c] |= 1;
    mockEntity[0xbb0] |= 1;
    assert((mockEntity[0xc3c] & 1) == 1);
    assert((mockEntity[0xbb0] & 1) == 1);

    // Test null/invalid safety
    HideAllyFromCutscene(0);
    HideAllyFromCutscene(0x10);

    std::puts("PASS: TestCutsceneVisibilityOverride completed.");
}

int main() {
    std::puts("=================================================");
    std::puts("  The Ashen Link - Native Cutscene Fix Tests");
    std::puts("=================================================");

    TestCutsceneFixLifecycleAndStats();
    TestCutsceneVisibilityOverride();

    std::puts("\nALL CUTSCENE FIX TESTS PASSED (100% OK).");
    return 0;
}
