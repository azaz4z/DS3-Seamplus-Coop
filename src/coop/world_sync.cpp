#include "world_sync.h"

namespace ds3sc::coop {

std::vector<std::uint8_t> WorldSync::EncodeEventFlag(EventFlagUpdate update) {
    std::vector<std::uint8_t> bytes(5);
    for (std::size_t i = 0; i != 4; ++i) bytes[i] = static_cast<std::uint8_t>(update.eventId >> (i * 8));
    bytes[4] = update.enabled ? 1 : 0;
    return bytes;
}

std::optional<EventFlagUpdate> WorldSync::DecodeEventFlag(
    std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() != 5 || payload[4] > 1) return std::nullopt;
    std::uint32_t eventId = 0;
    for (std::size_t i = 0; i != 4; ++i) eventId |= static_cast<std::uint32_t>(payload[i]) << (i * 8);
    return EventFlagUpdate{eventId, payload[4] != 0};
}

bool WorldSync::Apply(EventFlagUpdate update) {
    std::scoped_lock lock(mutex_);
    const auto found = flags_.find(update.eventId);
    if (found == flags_.end()) {
        flags_.emplace(update.eventId, update.enabled);
        return true;
    }
    if (found->second == update.enabled) return false;
    found->second = update.enabled;
    return true;
}

bool WorldSync::IsSet(std::uint32_t eventId) const {
    std::scoped_lock lock(mutex_);
    const auto found = flags_.find(eventId);
    return found != flags_.end() && found->second;
}

std::map<std::uint32_t, bool> WorldSync::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return flags_;
}

} // namespace ds3sc::coop
