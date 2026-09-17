#include "hit_sync.h"

#include <algorithm>
#include <array>

namespace ds3sc::coop {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'H', 'I', 'T', 'S'};

void Put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i != 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint32_t Get32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i != 4; ++i)
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8);
    return value;
}

void Put64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i != 8; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint64_t Get64(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i != 8; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
    return value;
}

} // namespace

bool HitSyncManager::IsValid(const HitSyncEvent& event) noexcept {
    // Must have attacker, valid entity, and non-zero sequence
    if (event.attackerSteamId == 0 || event.targetEntityId == 0 || event.sequenceNumber == 0) {
        return false;
    }
    // Damage cannot be absurd (maximum 500,000 to avoid malicious overflows)
    if (event.damage > 500'000) {
        return false;
    }
    return true;
}

std::vector<std::uint8_t> HitSyncManager::Encode(const HitSyncEvent& event) {
    if (!IsValid(event)) return {};

    std::vector<std::uint8_t> bytes(kPayloadSize, 0);
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin()); // 0..3: "HITS"

    Put64(bytes, 4, event.attackerSteamId);                 // 4..11: Attacker
    Put32(bytes, 12, event.targetEntityId);                // 12..15: Target ID
    Put32(bytes, 16, event.sequenceNumber);                // 16..19: Sequence
    Put32(bytes, 20, event.damage);                        // 20..23: Damage
    Put32(bytes, 24, event.targetHealthAfter);             // 24..27: Remaining HP
    bytes[28] = static_cast<std::uint8_t>(event.flags);    // 28: Flags
    Put64(bytes, 29, event.timestampMs);                   // 29..36: Timestamp
    return bytes;
}

std::optional<HitSyncEvent> HitSyncManager::Decode(std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() < 37) return std::nullopt;
    if (!std::equal(kMagic.begin(), kMagic.end(), payload.begin())) return std::nullopt;

    HitSyncEvent event;
    event.attackerSteamId = Get64(payload, 4);
    event.targetEntityId = Get32(payload, 12);
    event.sequenceNumber = Get32(payload, 16);
    event.damage = Get32(payload, 20);
    event.targetHealthAfter = Get32(payload, 24);
    event.flags = static_cast<HitSyncFlags>(payload[28]);
    event.timestampMs = Get64(payload, 29);

    if (!IsValid(event)) return std::nullopt;
    return event;
}

bool HitSyncManager::RegisterHit(const HitSyncEvent& event) noexcept {
    if (!IsValid(event)) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = std::make_pair(event.attackerSteamId, event.sequenceNumber);
    if (processedHits_.find(key) != processedHits_.end()) {
        return false; // Already processed (duplicate)
    }

    processedHits_.emplace(key, event.timestampMs);

    // Update target's known health if valid health was provided
    if (event.targetHealthAfter > 0 || HasFlag(event.flags, HitSyncFlags::hostConfirmed)) {
        entityHealth_[event.targetEntityId] = event.targetHealthAfter;
    } else {
        auto it = entityHealth_.find(event.targetEntityId);
        if (it != entityHealth_.end() && it->second > event.damage) {
            it->second -= event.damage;
        }
    }

    return true;
}

bool HitSyncManager::HasProcessed(std::uint64_t attacker, std::uint32_t sequence) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return processedHits_.find(std::make_pair(attacker, sequence)) != processedHits_.end();
}

std::optional<std::uint32_t> HitSyncManager::GetKnownHealth(std::uint32_t entityId) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entityHealth_.find(entityId);
    if (it != entityHealth_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool HitSyncManager::UpdateEntityHealth(std::uint32_t entityId, std::uint32_t currentHp) noexcept {
    if (entityId == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    entityHealth_[entityId] = currentHp;
    return true;
}

void HitSyncManager::PurgeOldRecords(std::uint64_t nowMs, std::uint64_t maxAgeMs) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = processedHits_.begin(); it != processedHits_.end(); ) {
        if (nowMs >= it->second && (nowMs - it->second) > maxAgeMs) {
            it = processedHits_.erase(it);
        } else {
            ++it;
        }
    }
}

void HitSyncManager::Clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    processedHits_.clear();
    entityHealth_.clear();
}

std::size_t HitSyncManager::ProcessedCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return processedHits_.size();
}

} // namespace ds3sc::coop
