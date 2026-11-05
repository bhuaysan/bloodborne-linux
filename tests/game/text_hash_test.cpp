// Differential tests of the hashes in src/game/text/hash.cpp.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "game/text/hash.hpp"
#include "harness/eboot.hpp"
#include "harness/harness.hpp"

namespace {

using bb::game::CharRange;
using bb::harness::Arena;
using bb::harness::Result;
using bb::test::EbootTest;

using HashBytesFn = std::uint64_t(const std::uint8_t*, std::size_t, std::uint64_t);

// `bytes` in the arena, passed with its size and `seed`.
auto bytes_with_seed(std::vector<std::uint8_t> bytes, std::uint64_t seed) {
    return [bytes = std::move(bytes), seed](Arena& a) {
        auto* data = a.zeroed<std::uint8_t>(bytes.size() + 1);
        if (!bytes.empty()) {
            std::memcpy(data, bytes.data(), bytes.size());
        }
        return std::tuple<const std::uint8_t*, std::size_t, std::uint64_t>{data, bytes.size(),
                                                                          seed};
    };
}

TEST_F(EbootTest, HashBytesBackwardMatches) {
    EXPECT_EQ(image().function_size(0x3233c0), std::optional<std::uint64_t>{40});
    std::vector<std::uint8_t> all(256);
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = static_cast<std::uint8_t>(i);
    }
    const std::vector<std::vector<std::uint8_t>> inputs = {
        {}, {0x00}, {0x41}, {0xff}, {0x01, 0x02}, {0x02, 0x01}, {0x80, 0x7f, 0xff, 0x00}, all};
    for (const auto& bytes : inputs) {
        for (std::uint64_t seed : {0ULL, 1ULL, 0x1003fULL, 0xffffffffffffffffULL}) {
            const Result r = harness().compare<HashBytesFn>(
                0x3233c0, &bb::game::hash_bytes_backward, bytes_with_seed(bytes, seed));
            EXPECT_TRUE(r) << bytes.size() << " bytes, seed " << seed << ": " << r.report();
        }
    }
}

// `text` in the arena, passed as a range; `reversed` swaps begin and end.
auto range_of(std::string_view text, bool reversed = false) {
    return [text, reversed](Arena& a) {
        auto* chars = static_cast<char*>(a.alloc(text.size() + 1));
        std::memcpy(chars, text.data(), text.size());
        auto* range = a.zeroed<CharRange>();
        range->begin = reversed ? chars + text.size() : chars;
        range->end = reversed ? chars : chars + text.size();
        return std::tuple<const CharRange*>{range};
    };
}

TEST_F(EbootTest, HashCharRangeMatches) {
    EXPECT_EQ(image().function_size(0x45d210), std::optional<std::uint64_t>{36});
    using namespace std::string_view_literals;
    for (std::string_view text : {""sv, "a"sv, "ab"sv, "ba"sv, "\x80"sv, "\xff\x7f\x01"sv,
                                  "a\0b"sv, "a fairly long name that overflows 64 bits"sv}) {
        const Result r = harness().compare<std::uint64_t(const CharRange*)>(
            0x45d210, &bb::game::hash_char_range, range_of(text));
        EXPECT_TRUE(r) << "text of " << text.size() << " chars: " << r.report();
    }
    const Result r = harness().compare<std::uint64_t(const CharRange*)>(
        0x45d210, &bb::game::hash_char_range, range_of("abc", true));
    EXPECT_TRUE(r) << "begin > end: " << r.report();
}

}  // namespace
