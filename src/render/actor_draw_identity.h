#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace ds3sc::render {

// ChrModel +0x20 contains a row-major affine 3x4 world transform in the
// verified game build. A nearby float3, stride or model type is not identity.
inline bool MatchesWorldTransform(const std::array<float, 12>& world,
                                  const void* data, std::size_t size) noexcept {
    if (!data || size < 48) return false;
    for (auto value : world) if (!std::isfinite(value)) return false;
    // Empty/default transforms are common in unrelated scene constants.
    if (std::abs(world[3]) + std::abs(world[7]) + std::abs(world[11]) < 0.01f) return false;
    for (std::size_t row = 0; row < 3; ++row) {
        float length = 0;
        for (std::size_t column = 0; column < 3; ++column)
            length += world[row * 4 + column] * world[row * 4 + column];
        if (length < 0.01f || length > 100.0f) return false;
    }
    const std::array<float, 16> transposed{
        world[0], world[4], world[8], 0,
        world[1], world[5], world[9], 0,
        world[2], world[6], world[10], 0,
        world[3], world[7], world[11], 1};
    auto matches = [](const float* candidate, const float* expected, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(candidate[i]) || std::abs(candidate[i] - expected[i]) > 0.0001f) return false;
        }
        return true;
    };
    for (std::size_t offset = 0; offset + 48 <= size; offset += 16) {
        std::array<float, 16> candidate{};
        const bool complete = offset + 64 <= size;
        std::memcpy(candidate.data(), static_cast<const std::byte*>(data) + offset, complete ? 64 : 48);
        if (matches(candidate.data(), world.data(), 12) ||
            (complete && matches(candidate.data(), transposed.data(), 16))) return true;
    }
    return false;
}
} // namespace ds3sc::render
