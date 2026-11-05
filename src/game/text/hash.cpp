#include "game/text/hash.hpp"

#include "hook/hook.hpp"

namespace bb::game {

// 0x003233c0
// TODO(verify): nothing calls it directly, so whether the third argument is a seed or a
// running hash of earlier data is unknown.
std::uint64_t hash_bytes_backward(const std::uint8_t* data, std::size_t size, std::uint64_t seed) {
    std::uint64_t h = seed;
    while (size != 0) {
        --size;
        h = h * 0x1003f + data[size];
    }
    return h;
}

// 0x0045d210
std::uint64_t hash_char_range(const CharRange* range) {
    std::uint64_t h = 0;
    for (const char* p = range->begin; p < range->end; ++p) {
        // The game's char is signed: bytes from 0x80 up subtract.
        const auto c = static_cast<std::int64_t>(static_cast<signed char>(*p));
        h = h * 31 + static_cast<std::uint64_t>(c);
    }
    return h;
}

}  // namespace bb::game

BB_REPLACE(0x003233c0, bb::game::hash_bytes_backward);
BB_REPLACE(0x0045d210, bb::game::hash_char_range);
