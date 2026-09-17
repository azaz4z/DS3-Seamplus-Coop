#pragma once

#include "packet.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ds3sc::network {

// Message identifiers recovered from the calls to networking.cpp::Send and
// networking.cpp::Receive. The names describe the observed payload shape and
// ownership, not a claim about the original private game type.
enum class MessageId : std::uint8_t {
    bulkState = 0x00,
    gameState = 0x0e,
    sessionState = 0x0f,
    playerData = 0x10,
    worldData = 0x11,
    mapState = 0x12,
    itemState = 0x13,
    playerReady = 0x14,
    sessionReady = 0x15,
    playerPosition = 0x16,
    playerRotation = 0x17,
    eventFlag = 0x18,
    playerDeath = 0x19,
    playerRespawn = 0x1a,
    playerLoad = 0x1b,
    playerLeave = 0x1c,
    worldRegion = 0x1d,
};

struct MessageSpec {
    SendType type;
    MessageId id;
    std::size_t minimumPayload;
    std::size_t maximumPayload;
    const char* name;
};

const MessageSpec* FindMessageSpec(SendType type, std::uint8_t id) noexcept;

// Returns false for a known message whose observed payload bounds are
// violated. Unknown messages remain accepted so a newer mod can interoperate
// with this reconstruction without silently truncating data.
bool ValidateMessage(const PacketView& packet, std::size_t maximumPayload) noexcept;

inline constexpr std::uint8_t ToByte(MessageId id) noexcept {
    return static_cast<std::uint8_t>(id);
}

} // namespace ds3sc::network
