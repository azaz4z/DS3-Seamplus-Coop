#pragma once
#include "packet.h"
#include <mutex>

namespace ds3sc::network {
struct PeerState {
    std::uint64_t steamId;
    std::uint64_t flags; // Original object: ID at +0, flags at +0x120.
};

// This boundary is NOT the Steamworks ABI. A real adapter and its initialization
// remain to be reconstructed and validated against the original interface.
class MessageTransport {
public:
    virtual ~MessageTransport() = default;
    virtual int Send(std::uint64_t peer, std::span<const std::uint8_t> packet,
                     int flags, int channel) = 0;
    // Steam adapters can reopen a closed P2P session here. Transports without
    // an explicit reconnect operation keep the conservative default.
    virtual bool Reconnect(std::uint64_t peer) { (void)peer; return false; }
};

class Sender final {
public:
    Sender(MessageTransport& transport, int primaryChannel, int alternateChannel)
        : transport_(transport), primaryChannel_(primaryChannel), alternateChannel_(alternateChannel) {}
    // Callers must provide stable snapshots of peers and the type-0 ID list.
    bool Send(std::span<const PeerState> peers, std::span<const std::uint32_t> type0Ids,
              std::uint64_t destination, std::uint32_t packetId, SendType type,
              std::span<const std::uint8_t> payload);
    bool Reconnect(std::uint64_t peer);
private:
    MessageTransport& transport_;
    int primaryChannel_;
    int alternateChannel_;
    std::mutex transportMutex_;
};
} // namespace ds3sc::network
