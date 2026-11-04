// SHA-256 as specified in FIPS 180-4 (docs/references.md).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace bb::common {

using Sha256Digest = std::array<std::uint8_t, 32>;

// Incremental hashing: update() any number of times, then finish() once.
class Sha256 {
public:
    void update(std::span<const std::byte> data);
    Sha256Digest finish();

private:
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

Sha256Digest sha256(std::span<const std::byte> data);

// Lower-case hex, as printed by sha256sum.
std::string to_hex(const Sha256Digest& digest);

}  // namespace bb::common
