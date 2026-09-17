#include "protocol.h"

#include <array>

namespace ds3sc::network {
namespace {

constexpr std::size_t kVariablePayload = 0x10000;

constexpr std::array<MessageSpec, 22> kMessages = {{
    {SendType::type2, MessageId::bulkState, 0, kVariablePayload, "bulk_state"},
    {SendType::type3, MessageId::gameState, 1, 1, "game_state"},
    {SendType::type3, MessageId::sessionState, 1, 1, "session_state"},
    {SendType::type3, MessageId::playerData, 0, kVariablePayload, "player_data"},
    {SendType::type3, MessageId::worldData, 0, kVariablePayload, "world_data"},
    {SendType::type3, MessageId::mapState, 0, 1, "map_state"},
    {SendType::type3, MessageId::itemState, 0, kVariablePayload, "item_state"},
    {SendType::type3, MessageId::playerReady, 1, 1, "player_ready"},
    {SendType::type3, MessageId::sessionReady, 1, 1, "session_ready"},
    {SendType::type3, MessageId::playerPosition, 8, 8, "player_position"},
    {SendType::type3, MessageId::playerRotation, 8, 8, "player_rotation"},
    {SendType::type3, MessageId::eventFlag, 5, 5, "event_flag"},
    {SendType::type3, MessageId::playerDeath, 4, 4, "player_death"},
    {SendType::type3, MessageId::playerRespawn, 8, 8, "player_respawn"},
    {SendType::type3, MessageId::playerLoad, 4, 4, "player_load"},
    {SendType::type3, MessageId::playerLeave, 1, 1, "player_leave"},
    {SendType::type3, MessageId::worldRegion, 2, 2, "world_region"},
    {SendType::type3, static_cast<MessageId>(0x02), 0, 0x49, "initial_state"},
    {SendType::type3, static_cast<MessageId>(0x03), 0, 0x49, "initial_state_ack"},
    {SendType::type3, static_cast<MessageId>(0x04), 1, 0x49, "connection_state"},
    {SendType::type3, static_cast<MessageId>(0x05), 4, 4, "character_state"},
    {SendType::type3, static_cast<MessageId>(0x06), 0x50, 0x50, "character_snapshot"},
}};

} // namespace

const MessageSpec* FindMessageSpec(SendType type, std::uint8_t id) noexcept {
    for (const auto& message : kMessages) {
        if (message.type == type && ToByte(message.id) == id) return &message;
    }
    return nullptr;
}

bool ValidateMessage(const PacketView& packet, std::size_t maximumPayload) noexcept {
    if (packet.payload.size() > maximumPayload) return false;
    const auto* spec = FindMessageSpec(packet.type, packet.id);
    if (!spec) return true;
    return packet.payload.size() >= spec->minimumPayload &&
           packet.payload.size() <= spec->maximumPayload;
}

} // namespace ds3sc::network
