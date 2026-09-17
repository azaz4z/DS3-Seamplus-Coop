#include "../src/extensions/counters/counters_extension.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <windows.h>

using namespace ds3sc::extensions;

int main() {
    std::cout << "[TEST] Starting unit tests for ContadoresExtension (Counters Module)..." << std::endl;

    // 1. Instance and initialization
    auto ext = std::make_shared<ContadoresExtension>();
    assert(std::string(ext->GetId()) == "counters" || std::string(ext->GetId()) == "contadores");
    assert(std::string(ext->GetName()) == "Counters");
    assert(ext->Initialize());

    // Reset before test to isolate environment
    ext->ResetStats();
    ContadoresSnapshot initialSnap = ext->GetStats();
    assert(initialSnap.deaths == 0);
    assert(initialSnap.kills == 0);
    assert(initialSnap.backstabsInflicted == 0);
    assert(initialSnap.backstabsReceived == 0);
    std::cout << "[TEST] Initialization and initial state at 0 OK." << std::endl;

    // 2. Increment verification
    ext->RecordDeath();
    ext->RecordDeath();
    ext->RecordKill();
    ext->RecordKill();
    ext->RecordKill();
    ext->RecordBackstabInflicted();
    ext->RecordBackstabReceived();
    ext->RecordBackstabReceived();

    ContadoresSnapshot snap = ext->GetStats();
    assert(snap.deaths == 2);
    assert(snap.kills == 3);
    assert(snap.backstabsInflicted == 1);
    assert(snap.backstabsReceived == 2);
    std::cout << "[TEST] Increments of deaths (2), kills (3), bs inflicted (1), bs received (2) OK." << std::endl;

    // 3. Verification of C API exports (new and backward compatibility)
    uint32_t cntDeaths = 0, cntKills = 0, cntBsInf = 0, cntBsRec = 0;
    ds3sc_get_counters(&cntDeaths, &cntKills, &cntBsInf, &cntBsRec);
    assert(cntDeaths == 2);
    assert(cntKills == 3);
    assert(cntBsInf == 1);
    assert(cntBsRec == 2);

    uint32_t cDeaths = 0, cKills = 0, cBsInf = 0, cBsRec = 0;
    ds3sc_get_contadores(&cDeaths, &cKills, &cBsInf, &cBsRec);
    assert(cDeaths == 2);
    assert(cKills == 3);
    assert(cBsInf == 1);
    assert(cBsRec == 2);

    uint32_t legacyDeaths = 0, legacyKills = 0, legacyBsInf = 0, legacyBsRec = 0;
    ds3sc_get_combat_stats(&legacyDeaths, &legacyKills, &legacyBsInf, &legacyBsRec);
    assert(legacyDeaths == 2);
    assert(legacyKills == 3);
    assert(legacyBsInf == 1);
    assert(legacyBsRec == 2);
    std::cout << "[TEST] C API ds3sc_get_counters, ds3sc_get_contadores, and ds3sc_get_combat_stats OK." << std::endl;

    // 4. Verification of overlay visibility
    assert(ext->IsOverlayVisible());
    ext->SetOverlayVisible(false);
    assert(!ext->IsOverlayVisible());
    ext->ToggleOverlay();
    assert(ext->IsOverlayVisible());

    ds3sc_set_counters_overlay_visible(0);
    assert(ds3sc_is_counters_overlay_visible() == 0);
    assert(ds3sc_is_contadores_overlay_visible() == 0);
    assert(ds3sc_is_combat_overlay_visible() == 0);
    ds3sc_set_counters_overlay_visible(1);
    assert(ds3sc_is_counters_overlay_visible() == 1);
    assert(ds3sc_is_contadores_overlay_visible() == 1);
    assert(ds3sc_is_combat_overlay_visible() == 1);
    std::cout << "[TEST] Overlay visibility toggling and C API OK." << std::endl;

    // 5. Verification of disk persistence
    ext->Shutdown(); // Forces save to ds3sc_stats.ini under [COUNTERS]

    ContadoresExtension ext2;
    assert(ext2.Initialize());
    ContadoresSnapshot loadedSnap = ext2.GetStats();
    assert(loadedSnap.deaths == 2);
    assert(loadedSnap.kills == 3);
    assert(loadedSnap.backstabsInflicted == 1);
    assert(loadedSnap.backstabsReceived == 2);
    std::cout << "[TEST] Disk persistence reload OK." << std::endl;

    // 6. ResetStats verification
    ext2.ResetStats();
    ContadoresSnapshot resetSnap = ext2.GetStats();
    assert(resetSnap.deaths == 0);
    assert(resetSnap.kills == 0);
    assert(resetSnap.backstabsInflicted == 0);
    assert(resetSnap.backstabsReceived == 0);

    // C API reset
    ext2.RecordDeath();
    assert(ext2.GetStats().deaths == 1);
    ds3sc_reset_counters();
    assert(ext2.GetStats().deaths == 0);
    ext2.Shutdown();
    std::cout << "[TEST] ResetStats and ds3sc_reset_counters OK." << std::endl;

    std::cout << "[PASS] All tests for counters module passed successfully!" << std::endl;
    return 0;
}
