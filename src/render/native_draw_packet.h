#pragma once
#include "actor_tracker.h"

namespace ds3sc::render {
// 59040's fifth argument is the 0xc0-byte submission options block built at
// d079c0. +0x0e permits batching: 57aa6 hashes/caches the material and 5c387
// selects the 138e10/b7f40 queues instead of the per-instance 59220 consumer.
// Copy it for the duration of submission; never change the engine's descriptor.
// The header is only 0x20 bytes: 5c0e0 also reads pointer fields through +0xb0
// and six aligned transform rows at +0x30..+0x80. Truncating those crashes DS3.
using NativeSubmissionOptions = std::array<std::byte, 0xc0>;
inline bool IndividualAllySubmission(const void* options,
                                    NativeSubmissionOptions& copy) noexcept {
    if (!SafeReadBytes(reinterpret_cast<std::uintptr_t>(options), copy.data(), copy.size())) return false;
    copy[0x0e] = std::byte{0};
    return true;
}
// Verified in DS3: d07d56 passes entity+0xd0 as the submission interface;
// 5c0eb saves it, and 5cd8e stores it at packet+0x48. 59220 dispatches
// command+0x80 as the FOURTH argument to 5cec0. packet+0x110 is a submission
// descriptor, not this interface. Neither FLVER resource pointer is identity.
inline std::uintptr_t PacketDrawEntity(std::uintptr_t packet) noexcept {
    if (!packet) return 0;
    std::uintptr_t iface = 0;
    if (SafeRead(packet + 0x48u, iface) && iface >= 0x10000u) {
        return iface >= 0xd0u ? (iface - 0xd0u) : iface;
    }
    return 0;
}
} // namespace ds3sc::render
