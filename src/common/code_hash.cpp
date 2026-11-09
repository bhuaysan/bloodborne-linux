#include "common/code_hash.hpp"

#include <array>
#include <bit>
#include <cstring>

namespace bb::common {

namespace {

constexpr std::uint64_t mul_a = 0x9e3779b97f4a7c15u;
constexpr std::uint64_t mul_b = 0xc2b2ae3d27d4eb4fu;

std::uint64_t mix(std::uint64_t h, std::uint64_t w) {
    return std::rotl((h ^ w) * mul_a, 29) * mul_b;
}

}  // namespace

std::uint64_t hash_code(std::span<const std::uint32_t> words) {
    // Four independent lanes over 32-byte blocks, so the multiplications overlap.
    std::array<std::uint64_t, 4> lane = {1, 2, 3, 4};
    const auto* p = reinterpret_cast<const unsigned char*>(words.data());
    std::size_t bytes = words.size() * sizeof(std::uint32_t);
    for (; bytes >= 32; bytes -= 32, p += 32) {
        for (std::size_t i = 0; i < 4; ++i) {
            std::uint64_t w;
            std::memcpy(&w, p + 8 * i, 8);
            lane[i] = mix(lane[i], w);
        }
    }
    std::uint64_t h = words.size();
    for (; bytes >= 4; bytes -= 4, p += 4) {
        std::uint32_t w;
        std::memcpy(&w, p, 4);
        h = mix(h, w);
    }
    for (const std::uint64_t l : lane) {
        h = mix(h, l);
    }
    return h ^ (h >> 32);
}

}  // namespace bb::common
