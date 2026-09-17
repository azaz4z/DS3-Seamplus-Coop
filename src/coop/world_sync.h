#pragma once

#include "../network/packet.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace ds3sc::coop {

struct EventFlagUpdate {
    std::uint32_t eventId = 0;
    bool enabled = false;
};

class WorldSync final {
public:
    // Packet 0x18 is observed with a four byte event id followed by a byte
    // state in the original send and receive paths.
    static std::vector<std::uint8_t> EncodeEventFlag(EventFlagUpdate update);
    static std::optional<EventFlagUpdate> DecodeEventFlag(
        std::span<const std::uint8_t> payload) noexcept;

    bool Apply(EventFlagUpdate update);
    [[nodiscard]] bool IsSet(std::uint32_t eventId) const;
    [[nodiscard]] std::map<std::uint32_t, bool> Snapshot() const;

private:
    mutable std::mutex mutex_;
    std::map<std::uint32_t, bool> flags_;
};

} // namespace ds3sc::coop
