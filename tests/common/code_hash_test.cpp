#include "common/code_hash.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace {

using bb::common::hash_code;

TEST(CodeHash, EqualWordsHashEqual) {
    std::vector<std::uint32_t> a(37), b;
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<std::uint32_t>(i * 2654435761u);
    b = a;
    EXPECT_EQ(hash_code(a), hash_code(b));
}

// A change of any one dword, in the blocks and in the tail, and a different length change the hash.
TEST(CodeHash, EveryDwordAndTheLengthCount) {
    std::vector<std::uint32_t> code(21, 0xbf810000u);
    const auto base = hash_code(code);
    for (std::size_t i = 0; i < code.size(); ++i) {
        auto changed = code;
        changed[i] ^= 1;
        EXPECT_NE(hash_code(changed), base) << i;
    }
    code.push_back(0);
    EXPECT_NE(hash_code(code), base);
    EXPECT_NE(hash_code({}), hash_code(std::vector<std::uint32_t>{0}));
}

}  // namespace
