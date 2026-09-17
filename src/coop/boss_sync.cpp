#include "boss_sync.h"

#include <algorithm>
#include <array>

namespace ds3sc::coop {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'B', 'O', 'S', 'S'};
constexpr std::size_t kPayloadSize = 25;

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

} // namespace

bool BossSync::IsValid(BossState state) noexcept {
    if (state.encounterId == 0 || state.epoch == 0 || state.revision == 0 ||
        state.maximumHealth == 0 || state.health > state.maximumHealth ||
        state.phase > BossPhase::defeated) return false;
    return state.phase != BossPhase::defeated || state.health == 0;
}

std::vector<std::uint8_t> BossSync::Encode(BossState state) {
    if (!IsValid(state)) return {};
    std::vector<std::uint8_t> bytes(kPayloadSize, 0);
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    Put32(bytes, 4, state.encounterId);
    Put32(bytes, 8, state.epoch);
    Put32(bytes, 12, state.revision);
    bytes[16] = static_cast<std::uint8_t>(state.phase);
    Put32(bytes, 17, state.health);
    Put32(bytes, 21, state.maximumHealth);
    return bytes;
}

std::optional<BossState> BossSync::Decode(std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() != kPayloadSize ||
        !std::equal(kMagic.begin(), kMagic.end(), payload.begin())) return std::nullopt;
    BossState state{Get32(payload, 4), Get32(payload, 8), Get32(payload, 12),
                    static_cast<BossPhase>(payload[16]), Get32(payload, 17), Get32(payload, 21)};
    return IsValid(state) ? std::optional<BossState>{state} : std::nullopt;
}

bool BossSync::ApplyAuthoritative(BossState state) {
    if (!IsValid(state)) return false;
    std::scoped_lock lock(mutex_);
    const auto found = states_.find(state.encounterId);
    if (found == states_.end()) {
        states_.emplace(state.encounterId, state);
        return true;
    }
    const auto& current = found->second;
    if (state.epoch < current.epoch) return false;
    if (state.epoch == current.epoch) {
        if (state.revision <= current.revision || state.phase < current.phase ||
            current.phase == BossPhase::defeated) return false;
        if (state.phase == current.phase && state.health > current.health) return false;
    }
    found->second = state;
    return true;
}

std::optional<BossState> BossSync::Get(std::uint32_t encounterId) const {
    std::scoped_lock lock(mutex_);
    const auto found = states_.find(encounterId);
    return found == states_.end() ? std::nullopt : std::optional<BossState>{found->second};
}

std::vector<BossState> BossSync::Snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<BossState> states;
    states.reserve(states_.size());
    for (const auto& [id, state] : states_) {
        (void)id;
        states.push_back(state);
    }
    return states;
}

void BossSync::Clear() noexcept {
    std::scoped_lock lock(mutex_);
    states_.clear();
}

} // namespace ds3sc::coop
