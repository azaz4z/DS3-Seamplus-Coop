#pragma once

#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>

namespace ds3sc::network {

enum class DispatchResult { delivered, ignoredUnknown, rejectedFrame, rejectedPayload };

class PacketRouter final {
public:
    using Handler = std::function<void(std::uint64_t, const PacketView&)>;

    void Register(SendType type, std::uint8_t id, Handler handler);
    DispatchResult Dispatch(std::uint64_t peer, std::span<const std::uint8_t> bytes,
                             std::size_t maximumPayload = 0x10000) const noexcept;

private:
    static std::uint16_t Key(SendType type, std::uint8_t id) noexcept;
    std::map<std::uint16_t, Handler> handlers_;
};

} // namespace ds3sc::network
