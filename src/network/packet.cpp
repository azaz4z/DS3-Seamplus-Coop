#include "packet.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ds3sc::network {
std::optional<int> SendFlags(SendType type, bool idInType0List) noexcept {
    switch (type) {
    case SendType::type0: return idInType0List ? 5 : 9;
    case SendType::type1: return 9;
    case SendType::type2: return 5;
    case SendType::type3: return 9;
    default: return std::nullopt;
    }
}

std::vector<std::uint8_t> Encode(SendType type, std::uint8_t id,
                               std::span<const std::uint8_t> payload) {
    if (!SendFlags(type, false)) throw std::invalid_argument("Unknown packet send type");
    // Original transport accepts a signed 32-bit total size. Reject overflow
    // before allocating; this explicit guard is a reconstruction improvement.
    constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) - headerSize;
    if (payload.size() > limit) throw std::length_error("Packet exceeds transport size");
    std::vector<std::uint8_t> bytes(headerSize + payload.size());
    bytes[0] = static_cast<std::uint8_t>(type);
    bytes[1] = id;
    bytes[2] = 0;
    const auto length = static_cast<std::uint32_t>(payload.size());
    for (unsigned i = 0; i < 4; ++i) bytes[3 + i] = static_cast<std::uint8_t>(length >> (i * 8));
    std::copy(payload.begin(), payload.end(), bytes.begin() + headerSize);
    return bytes;
}

std::optional<PacketView> Decode(std::span<const std::uint8_t> bytes,
                                std::size_t maximumPayload) noexcept {
    if (bytes.size() < headerSize || bytes[0] > 3) return std::nullopt;
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i) length |= static_cast<std::uint32_t>(bytes[3 + i]) << (i * 8);
    // Exact-size checks are visible in receive routines at RVAs 0x68970 and
    // 0x68B10. A caller-defined size cap and minimum header check are additions.
    if (length > maximumPayload || length != bytes.size() - headerSize) return std::nullopt;
    return PacketView{static_cast<SendType>(bytes[0]), bytes[1], bytes[2], bytes.subspan(headerSize)};
}
} // namespace ds3sc::network
