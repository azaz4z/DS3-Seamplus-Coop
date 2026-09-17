#include "sender.h"
#include <algorithm>

namespace ds3sc::network {
// Independent reconstruction of RVA 0x68C40. Allocation and transport internals
// use C++ standard facilities; peer selection, bytes, flags and channel match
// the observed instructions. Not yet connected to the game or Steam.
bool Sender::Send(std::span<const PeerState> peers, std::span<const std::uint32_t> type0Ids,
                  std::uint64_t destination, std::uint32_t packetId, SendType type,
                  std::span<const std::uint8_t> payload) {
    const auto peer = std::find_if(peers.begin(), peers.end(), [&](const PeerState& item) {
        return (item.flags & 1) && item.steamId == destination;
    });
    if (peer == peers.end() || !(peer->flags & 2)) return false;
    const bool listed = std::find(type0Ids.begin(), type0Ids.end(), packetId) != type0Ids.end();
    const auto flags = SendFlags(type, listed);
    if (!flags) return false;
    // Full 32-bit ID is used for list lookup, low byte is placed on the wire.
    const auto packet = Encode(type, static_cast<std::uint8_t>(packetId), payload);
    const int channel = type == SendType::type3 ? alternateChannel_ : primaryChannel_;
    const std::scoped_lock lock(transportMutex_);
    return transport_.Send(destination, packet, *flags, channel) == 1;
}

bool Sender::Reconnect(std::uint64_t peer) {
    const std::scoped_lock lock(transportMutex_);
    return transport_.Reconnect(peer);
}
} // namespace ds3sc::network
