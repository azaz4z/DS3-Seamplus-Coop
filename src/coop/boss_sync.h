#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace ds3sc::coop {

enum class BossPhase : std::uint8_t {
    dormant = 0,
    entering = 1,
    active = 2,
    phaseTwo = 3,
    defeated = 4,
};

struct BossState {
    std::uint32_t encounterId = 0;
    std::uint32_t epoch = 0;
    std::uint32_t revision = 0;
    BossPhase phase = BossPhase::dormant;
    std::uint32_t health = 0;
    std::uint32_t maximumHealth = 0;
};

class BossSync final {
public:
    static std::vector<std::uint8_t> Encode(BossState state);
    static std::optional<BossState> Decode(std::span<const std::uint8_t> payload) noexcept;
    static bool IsValid(BossState state) noexcept;

    // Both local host updates and remote host snapshots pass through the same
    // monotonic rules. A new epoch represents a fresh encounter attempt.
    bool ApplyAuthoritative(BossState state);
    [[nodiscard]] std::optional<BossState> Get(std::uint32_t encounterId) const;
    [[nodiscard]] std::vector<BossState> Snapshot() const;
    void Clear() noexcept;

private:
    mutable std::mutex mutex_;
    std::map<std::uint32_t, BossState> states_;
};

} // namespace ds3sc::coop
