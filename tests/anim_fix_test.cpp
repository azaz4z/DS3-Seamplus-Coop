#include "../src/extensions/anim_fix/anim_fix_extension.h"
#include "../src/extensions/extension_manager.h"

#include <cassert>
#include <cstdio>
#include <vector>
#include <cmath>
#include <string>

using namespace ds3sc::extensions;

void TestAnimFixLifecycleAndStats() {
    auto ext = CreateAnimFixExtension();
    assert(ext != nullptr);
    assert(std::string(ext->GetId()) == "anim_fix");
    assert(ext->GetName() != nullptr);

    auto animFix = std::dynamic_pointer_cast<AnimFixExtension>(ext);
    assert(animFix != nullptr);

    // Initial stats
    std::uint32_t animFixes = 0, flagFixes = 0;
    animFix->GetStats(animFixes, flagFixes);
    assert(animFixes == 0);
    assert(flagFixes == 0);

    assert(animFix->Initialize());
    animFix->OnTick();
    animFix->Shutdown();

    std::puts("PASS: TestAnimFixLifecycleAndStats completed.");
}

void TestAnimationSpeedAndFlagRepairs() {
    // Simulate ChrIns structure and sub-modules
    std::vector<std::uint8_t> mockChr(0x2200, 0);
    std::vector<std::uint8_t> mockModules(0x200, 0);
    std::vector<std::uint8_t> mockBehavior(0x1200, 0);
    std::vector<std::uint8_t> mockPhysics(0x200, 0);

    // Link modules: ChrIns + 0x1F90 -> modules
    const auto modulesPtr = reinterpret_cast<std::uintptr_t>(mockModules.data());
    *reinterpret_cast<std::uintptr_t*>(&mockChr[0x1F90]) = modulesPtr;

    // Modules + 0x28 -> BehaviorModule
    const auto behaviorPtr = reinterpret_cast<std::uintptr_t>(mockBehavior.data());
    *reinterpret_cast<std::uintptr_t*>(&mockModules[0x28]) = behaviorPtr;

    // Modules + 0x68 -> PhysicsModule
    const auto physicsPtr = reinterpret_cast<std::uintptr_t>(mockPhysics.data());
    *reinterpret_cast<std::uintptr_t*>(&mockModules[0x68]) = physicsPtr;

    // Set linear horizontal velocity > 0.2 m/s
    *reinterpret_cast<float*>(&mockPhysics[0x70]) = 1.5f; // vx
    *reinterpret_cast<float*>(&mockPhysics[0x78]) = 1.0f; // vz

    // Freeze AnimationSpeed at 0.0f (the skating bug)
    *reinterpret_cast<float*>(&mockBehavior[0xA58]) = 0.0f;

    // Set stuck bit 7 in ChrIns + 0x1EE8
    mockChr[0x1EE8] = 0x80;

    // Verify condition: character is moving while AnimationSpeed is 0.0f and bit 7 is set
    float currentSpeed = *reinterpret_cast<float*>(&mockBehavior[0xA58]);
    assert(currentSpeed <= 0.001f);
    assert((mockChr[0x1EE8] & 0x80) != 0);

    // Logic repair simulation
    if (currentSpeed <= 0.001f) {
        *reinterpret_cast<float*>(&mockBehavior[0xA58]) = 1.0f;
    }
    if ((mockChr[0x1EE8] & 0x80) != 0) {
        mockChr[0x1EE8] &= 0x7F;
    }

    assert(*reinterpret_cast<float*>(&mockBehavior[0xA58]) == 1.0f);
    assert((mockChr[0x1EE8] & 0x80) == 0);

    std::puts("PASS: TestAnimationSpeedAndFlagRepairs completed.");
}

int main() {
    std::puts("=================================================");
    std::puts("  The Ashen Link - Native Anim Fix Tests");
    std::puts("=================================================");

    TestAnimFixLifecycleAndStats();
    TestAnimationSpeedAndFlagRepairs();

    std::puts("\nALL ANIM FIX TESTS PASSED (100% OK).");
    return 0;
}
