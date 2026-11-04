#include "common/sha256.hpp"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using bb::common::Sha256;
using bb::common::sha256;
using bb::common::to_hex;

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

// Test vectors of FIPS 180-2, appendix B (docs/references.md).
TEST(Sha256, OneBlockMessage) {
    EXPECT_EQ(to_hex(sha256(bytes("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, MultiBlockMessage) {
    EXPECT_EQ(to_hex(sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, LongMessage) {
    std::string million(1'000'000, 'a');
    EXPECT_EQ(to_hex(sha256(bytes(million))),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Feeding a message in two parts gives the same digest for every split point.
TEST(Sha256, IncrementalUpdatesMatchOneShot) {
    std::string message;
    for (int i = 0; i < 200; ++i) {
        message += static_cast<char>('A' + i % 26);
    }
    for (std::size_t len = 0; len <= message.size(); len += 7) {
        std::string_view part(message.data(), len);
        auto want = sha256(bytes(part));
        for (std::size_t split = 0; split <= len; ++split) {
            Sha256 h;
            h.update(bytes(part.substr(0, split)));
            h.update(bytes(part.substr(split)));
            ASSERT_EQ(h.finish(), want) << "length " << len << ", split at " << split;
        }
    }
}

}  // namespace
