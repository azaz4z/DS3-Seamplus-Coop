#include "../src/coop/hit_sync.h"
#include "../src/extensions/hit_sync/hit_sync_extension.h"
#include "../src/extensions/extension_manager.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <vector>

using namespace ds3sc::coop;
using namespace ds3sc::extensions;

void TestPacketEncoding() {
    HitSyncEvent event{};
    event.attackerSteamId = 0x0110000100002222ull;
    event.targetEntityId = 3100800;
    event.sequenceNumber = 42;
    event.damage = 1250;
    event.targetHealthAfter = 4500;
    event.flags = HitSyncFlags::critical | HitSyncFlags::hostConfirmed;
    event.timestampMs = 123456789ull;

    assert(HitSyncManager::IsValid(event));

    const auto encoded = HitSyncManager::Encode(event);
    assert(encoded.size() == HitSyncManager::kPayloadSize);

    // Verify "HITS" magic header
    assert(encoded[0] == 'H' && encoded[1] == 'I' && encoded[2] == 'T' && encoded[3] == 'S');

    const auto decoded = HitSyncManager::Decode(encoded);
    assert(decoded.has_value());
    assert(decoded->attackerSteamId == event.attackerSteamId);
    assert(decoded->targetEntityId == event.targetEntityId);
    assert(decoded->sequenceNumber == event.sequenceNumber);
    assert(decoded->damage == event.damage);
    assert(decoded->targetHealthAfter == event.targetHealthAfter);
    assert(decoded->flags == event.flags);
    assert(decoded->timestampMs == event.timestampMs);

    // Rejection tests for corrupted or truncated packets
    std::vector<std::uint8_t> truncated(encoded.begin(), encoded.begin() + 10);
    assert(!HitSyncManager::Decode(truncated).has_value());

    std::vector<std::uint8_t> badMagic = encoded;
    badMagic[0] = 'X';
    assert(!HitSyncManager::Decode(badMagic).has_value());

    // Rejection of anomalous damage / overflow
    HitSyncEvent badDmg = event;
    badDmg.damage = 1'000'000;
    assert(!HitSyncManager::IsValid(badDmg));
    assert(HitSyncManager::Encode(badDmg).empty());

    // Rejection of entity ID 0 or attacker 0
    HitSyncEvent zeroEntity = event;
    zeroEntity.targetEntityId = 0;
    assert(!HitSyncManager::IsValid(zeroEntity));

    std::puts("PASS: TestPacketEncoding completed.");
}

void TestHitDeduplicationAndState() {
    HitSyncManager manager;
    assert(manager.ProcessedCount() == 0);

    HitSyncEvent hit1{};
    hit1.attackerSteamId = 1001;
    hit1.targetEntityId = 5000;
    hit1.sequenceNumber = 1;
    hit1.damage = 300;
    hit1.timestampMs = 10'000;

    // First hit must be registered
    assert(manager.RegisterHit(hit1));
    assert(manager.HasProcessed(1001, 1));
    assert(manager.ProcessedCount() == 1);

    // Identical duplicate (same attacker and sequence) must be rejected
    assert(!manager.RegisterHit(hit1));
    assert(manager.ProcessedCount() == 1);

    // Next sequence from same attacker must be accepted
    HitSyncEvent hit2 = hit1;
    hit2.sequenceNumber = 2;
    hit2.damage = 350;
    hit2.timestampMs = 10'200;
    assert(manager.RegisterHit(hit2));
    assert(manager.ProcessedCount() == 2);

    // Another attacker with sequence 1 must be accepted (different attacker)
    HitSyncEvent hit3 = hit1;
    hit3.attackerSteamId = 1002;
    hit3.timestampMs = 10'250;
    assert(manager.RegisterHit(hit3));
    assert(manager.ProcessedCount() == 3);

    // Entity health
    manager.UpdateEntityHealth(5000, 2000);
    assert(manager.GetKnownHealth(5000).value() == 2000);

    // Clean up old records
    manager.PurgeOldRecords(26'000, 15'000); // hit1 (10'000) is 16s old -> purged
    assert(!manager.HasProcessed(1001, 1));

    manager.Clear();
    assert(manager.ProcessedCount() == 0);
    assert(!manager.GetKnownHealth(5000).has_value());

    std::puts("PASS: TestHitDeduplicationAndState completed.");
}

void TestNoDamageAndNoHitBitmasks() {
    // Simulate DS3 flag fields to check bit arithmetic
    // ChrData + 0x1C0: NoDamage flag at bit 1 (0x02)
    std::uint8_t simulatedNoDmgByte = 0xFF;
    constexpr std::uint8_t kNoDamageMask = 0x02;
    assert((simulatedNoDmgByte & kNoDamageMask) != 0);
    simulatedNoDmgByte &= ~kNoDamageMask;
    assert((simulatedNoDmgByte & kNoDamageMask) == 0);
    assert(simulatedNoDmgByte == 0xFD);

    // ChrIns + 0x1EE8: NoHit flag at bit 5 (1 << 5 = 0x20)
    std::uint32_t simulatedFlags = 0xFFFFFFFFu;
    constexpr std::uint32_t kNoHitMask = 1u << 5;
    assert((simulatedFlags & kNoHitMask) != 0);
    simulatedFlags &= ~kNoHitMask;
    assert((simulatedFlags & kNoHitMask) == 0);
    assert(simulatedFlags == 0xFFFFFFDFu);

    std::puts("PASS: TestNoDamageAndNoHitBitmasks completed.");
}

void TestExtensionLifecycle() {
    auto extension = CreateHitSyncExtension();
    assert(extension != nullptr);
    assert(std::string(extension->GetId()) == "hit_sync");
    assert(std::string(extension->GetName()).find("Hit") != std::string::npos);

    assert(extension->Initialize());
    extension->OnTick();

    auto hitSyncExt = std::dynamic_pointer_cast<HitSyncExtension>(extension);
    assert(hitSyncExt != nullptr);
    assert(hitSyncExt->IsEnabled());

    hitSyncExt->SetEnabled(false);
    assert(!hitSyncExt->IsEnabled());
    hitSyncExt->SetEnabled(true);

    std::uint32_t hits = 0, ghosts = 0, enemies = 0;
    hitSyncExt->GetStats(hits, ghosts, enemies);
    assert(hits == 0);

    extension->Shutdown();

    std::puts("PASS: TestExtensionLifecycle completed.");
}

int main() {
    std::puts("Starting hit_sync unit tests...");
    TestPacketEncoding();
    TestHitDeduplicationAndState();
    TestNoDamageAndNoHitBitmasks();
    TestExtensionLifecycle();
    std::puts("\nALL HIT_SYNC TESTS PASSED SUCCESSFULLY.");
    return 0;
}
