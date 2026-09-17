#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace ds3sc::coop {

// Network hit modes
enum class HitSyncFlags : std::uint8_t {
    none = 0,
    critical = 1 << 0,
    stagger = 1 << 1,
    retransmit = 1 << 2,
    hostConfirmed = 1 << 3,
};

inline constexpr HitSyncFlags operator|(HitSyncFlags a, HitSyncFlags b) noexcept {
    return static_cast<HitSyncFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

inline constexpr bool HasFlag(HitSyncFlags flags, HitSyncFlags test) noexcept {
    return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(test)) != 0;
}

// Hit event and damage record
struct HitSyncEvent {
    std::uint64_t attackerSteamId = 0;  // Attacker Steam identity or session ID
    std::uint32_t targetEntityId = 0;   // Target entity / character ID (e.g. 3100800)
    std::uint32_t sequenceNumber = 0;   // Incremental sequence for deduplication
    std::uint32_t damage = 0;           // Raw damage calculated by attacker
    std::uint32_t targetHealthAfter = 0;// Remaining health calculated/confirmed (0 if unknown)
    HitSyncFlags flags = HitSyncFlags::none;
    std::uint64_t timestampMs = 0;      // Local hit timestamp
};

// Manager for enemy hit sync and damage validation
class HitSyncManager final {
public:
    static constexpr std::size_t kPayloadSize = 37; // 4 magic + 8 + 4 + 4 + 4 + 4 + 1 + 8

    // Packet on-wire serialization / deserialization
    static std::vector<std::uint8_t> Encode(const HitSyncEvent& event);
    static std::optional<HitSyncEvent> Decode(std::span<const std::uint8_t> payload) noexcept;
    static bool IsValid(const HitSyncEvent& event) noexcept;

    // Registers a hit and determines if it is new or duplicate
    bool RegisterHit(const HitSyncEvent& event) noexcept;

    // Checks if a hit has already been processed (by attacker and sequence)
    [[nodiscard]] bool HasProcessed(std::uint64_t attacker, std::uint32_t sequence) const noexcept;

    // Queries latest known health for an entity
    [[nodiscard]] std::optional<std::uint32_t> GetKnownHealth(std::uint32_t entityId) const noexcept;

    // Updates authoritative health of an entity (monotonically decreasing in combat)
    bool UpdateEntityHealth(std::uint32_t entityId, std::uint32_t currentHp) noexcept;

    // Purges old deduplication records (called periodically)
    void PurgeOldRecords(std::uint64_t nowMs, std::uint64_t maxAgeMs = 15'000) noexcept;

    // Complete cleanup when changing zone or session
    void Clear() noexcept;

    [[nodiscard]] std::size_t ProcessedCount() const noexcept;

private:
    mutable std::mutex mutex_;
    // Map: (attackerSteamId, sequenceNumber) -> timestampMs
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::uint64_t> processedHits_;
    // Map: entityId -> lastKnownHealth
    std::map<std::uint32_t, std::uint32_t> entityHealth_;
};

} // namespace ds3sc::coop
