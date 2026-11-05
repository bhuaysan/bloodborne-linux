// Hashes over bytes and characters that the game computes in small leaf functions.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bb::game {

// A range of characters [begin, end), not NUL-terminated.
struct CharRange {
    const char* begin;
    const char* end;
};

// Folds `size` bytes of `data` into `seed`, last byte first: h = h * 0x1003f + byte. Returns
// `seed` unchanged when `size` is 0.
std::uint64_t hash_bytes_backward(const std::uint8_t* data, std::size_t size, std::uint64_t seed);

// Folds the characters of `range` into 0, first character first: h = h * 31 + c, where c is
// the character sign-extended to 64 bits. Returns 0 for an empty range or one with
// begin > end.
std::uint64_t hash_char_range(const CharRange* range);

}  // namespace bb::game
