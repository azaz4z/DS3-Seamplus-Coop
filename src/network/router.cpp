#include "router.h"

#include <stdexcept>
#include <utility>

namespace ds3sc::network {

std::uint16_t PacketRouter::Key(SendType type, std::uint8_t id) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(type)) << 8 | id;
}

void PacketRouter::Register(SendType type, std::uint8_t id, Handler handler) {
    if (!handler) throw std::invalid_argument("Packet handler cannot be empty");
    handlers_[Key(type, id)] = std::move(handler);
}

DispatchResult PacketRouter::Dispatch(std::uint64_t peer,
                                     std::span<const std::uint8_t> bytes,
                                     std::size_t maximumPayload) const noexcept {
    const auto packet = Decode(bytes, maximumPayload);
    if (!packet) return DispatchResult::rejectedFrame;
    if (!ValidateMessage(*packet, maximumPayload)) return DispatchResult::rejectedPayload;
    const auto found = handlers_.find(Key(packet->type, packet->id));
    if (found == handlers_.end()) return DispatchResult::ignoredUnknown;
    try {
        found->second(peer, *packet);
    } catch (...) {
        return DispatchResult::rejectedPayload;
    }
    return DispatchResult::delivered;
}

} // namespace ds3sc::network
