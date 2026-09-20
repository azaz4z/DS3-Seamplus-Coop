#include "../src/extensions/combat_stats/combat_stats_extension.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <windows.h>

using namespace ds3sc::extensions;

int main() {
    std::cout << "[TEST] Starting unit tests for CombatStatsExtension..." << std::endl;

    // 1. Instance and initialization
    auto ext = std::make_shared<CombatStatsExtension>();
    assert(std::string(ext->GetId()) == "counters" || std::string(ext->GetId()) == "combat_stats" || std::string(ext->GetId()) == "contadores");
    assert(ext->GetName() != nullptr);
    assert(ext->Initialize());

    // Reset before test to isolate the environment
    ext->ResetStats();
    CombatStatsSnapshot initialSnap = ext->GetStats();
    assert(initialSnap.deaths == 0);
    assert(initialSnap.kills == 0);
    assert(initialSnap.backstabsInflicted == 0);
    assert(initialSnap.backstabsReceived == 0);
    std::cout << "[TEST] Initialization and initial state at 0 OK." << std::endl;

    // 2. Check increments
    ext->RecordDeath();
    ext->RecordDeath();
    ext->RecordKill();
    ext->RecordKill();
    ext->RecordKill();
    ext->RecordBackstabInflicted();
    ext->RecordBackstabReceived();
    ext->RecordBackstabReceived();

    CombatStatsSnapshot snap = ext->GetStats();
    assert(snap.deaths == 2);
    assert(snap.kills == 3);
    assert(snap.backstabsInflicted == 1);
    assert(snap.backstabsReceived == 2);
    std::cout << "[TEST] Increments of deaths (2), kills (3), bs inflicted (1), bs received (2) OK." << std::endl;

    // 3. Check C API exports
    uint32_t cDeaths = 0, cKills = 0, cBsInf = 0, cBsRec = 0;
    ds3sc_get_combat_stats(&cDeaths, &cKills, &cBsInf, &cBsRec);
    assert(cDeaths == 2);
    assert(cKills == 3);
    assert(cBsInf == 1);
    assert(cBsRec == 2);
    std::cout << "[TEST] C API ds3sc_get_combat_stats OK." << std::endl;

    // 4. Check overlay visibility
    ext->SetOverlayVisible(true);
    assert(ext->IsOverlayVisible());
    ext->SetOverlayVisible(false);
    assert(!ext->IsOverlayVisible());
    ext->ToggleOverlay();
    assert(ext->IsOverlayVisible());

    ds3sc_set_combat_overlay_visible(0);
    assert(ds3sc_is_combat_overlay_visible() == 0);
    ds3sc_set_combat_overlay_visible(1);
    assert(ds3sc_is_combat_overlay_visible() == 1);
    std::cout << "[TEST] Overlay visibility toggling and C API OK." << std::endl;

    // 5. Check disk persistence
    ext->Shutdown(); // Force save to ds3sc_stats.ini

    CombatStatsExtension ext2;
    assert(ext2.Initialize());
    CombatStatsSnapshot loadedSnap = ext2.GetStats();
    assert(loadedSnap.deaths == 2);
    assert(loadedSnap.kills == 3);
    assert(loadedSnap.backstabsInflicted == 1);
    assert(loadedSnap.backstabsReceived == 2);
    std::cout << "[TEST] Disk persistence (reload after Shutdown/Initialize) OK." << std::endl;

    // 6. Check ResetStats
    ext2.ResetStats();
    CombatStatsSnapshot resetSnap = ext2.GetStats();
    assert(resetSnap.deaths == 0);
    assert(resetSnap.kills == 0);
    assert(resetSnap.backstabsInflicted == 0);
    assert(resetSnap.backstabsReceived == 0);

    // C API reset
    ext2.RecordDeath();
    assert(ext2.GetStats().deaths == 1);
    ds3sc_reset_combat_stats();
    assert(ext2.GetStats().deaths == 0);
    ext2.Shutdown();
    std::cout << "[TEST] ResetStats and ds3sc_reset_combat_stats OK." << std::endl;

    std::cout << "[PASS] All combat statistics unit tests finished successfully!" << std::endl;
    return 0;
}
