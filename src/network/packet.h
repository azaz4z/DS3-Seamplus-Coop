#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ds3sc::network {
// Numeric names are intentional: complete game semantics are not yet recovered.
enum class SendType : std::uint8_t { type0, type1, type2, type3 };
inline constexpr std::size_t headerSize = 7;

struct PacketView {
    SendType type;
    std::uint8_t id;
    std::uint8_t auxiliary; // Send routine writes zero. Other uses remain unknown.
    std::span<const std::uint8_t> payload; // Borrows the caller's buffer.
};

// Layout recovered from ds3sc.dll RVA 0x68C40, stores at 0x68D59..0x68D64.
// Length is an unaligned little-endian uint32 at byte 3; no native struct casts.
std::vector<std::uint8_t> Encode(SendType type, std::uint8_t id,
                               std::span<const std::uint8_t> payload);
std::optional<PacketView> Decode(std::span<const std::uint8_t> bytes,
                                std::size_t maximumPayload) noexcept;
std::optional<int> SendFlags(SendType type, bool idInType0List) noexcept;
} // namespace ds3sc::network
