#include "hle/net/net.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "loader/image.hpp"
#include "loader/relocate.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::hle::net;

// The counters are global to the process, so the tests compare ids with each other, not with
// fixed values.

TEST(HleNet, InitSucceedsEveryTime) {
    EXPECT_EQ(sceNetInit(), 0);
    EXPECT_EQ(sceNetInit(), 0);
}

TEST(HleNet, InitChainHandsOutDistinctPositiveIds) {
    // The order of 0xfd1ffa..0xfd2032, twice, as two of the game's HTTP objects would.
    for (int round = 0; round < 2; ++round) {
        SCOPED_TRACE(round);
        const std::int32_t pool = sceNetPoolCreate("FD4HttpMan", 0x10000, 0);
        const std::int32_t pool2 = sceNetPoolCreate("ResolveDomainName", 0x1000, 0);
        EXPECT_GT(pool, 0);
        EXPECT_NE(pool, pool2);
        const std::int32_t ssl = sceSslInit(0x40000);
        EXPECT_GT(ssl, 0);
        const std::int32_t http = sceHttpInit(pool, ssl, 0x40000);
        EXPECT_GT(http, 0);
        EXPECT_NE(sceHttpInit(pool, ssl, 0x40000), http);
        EXPECT_NE(sceSslInit(0x40000), ssl);
    }
}

TEST(HleNet, TemplateCallsSucceed) {
    const std::int32_t http = sceHttpInit(1, 1, 0x40000);
    const std::int32_t a = sceHttpCreateTemplate(http, "agent", 2, 1);
    const std::int32_t b = sceHttpCreateTemplate(http, "agent", 2, 1);
    EXPECT_GT(a, 0);  // 0xfd1dfa: only ids above 0 are deleted
    EXPECT_NE(a, b);
    EXPECT_EQ(http_template_set_int(a, 1), 0);
    EXPECT_EQ(http_template_set_string_pair(a, "name", "value", 0), 0);
}

TEST(HleNet, CreateEpollWritesDistinctQwordHandles) {
    std::array<std::uint64_t, 3> handles{~0ULL, ~0ULL, 0x1234};
    EXPECT_EQ(sceHttpCreateEpoll(1, &handles[0]), 0);
    EXPECT_EQ(sceHttpCreateEpoll(1, &handles[1]), 0);
    EXPECT_GT(handles[0], 0U);
    EXPECT_LT(handles[0], 0x10000U);  // all 8 bytes written, and small
    EXPECT_NE(handles[0], handles[1]);
    EXPECT_EQ(handles[2], 0x1234U);
}

TEST(HleNet, NetCtlReportsDisconnected) {
    std::int32_t state = 3;
    EXPECT_EQ(sceNetCtlGetState(&state), 0);
    EXPECT_EQ(state, 0);

    std::array<unsigned char, 0x100> info;
    info.fill(0xa5);
    EXPECT_LT(sceNetCtlGetInfo(14, info.data()), 0);
    for (const unsigned char byte : info) {
        ASSERT_EQ(byte, 0xa5);
    }
}

TEST(HleNet, NpReportsSignedOut) {
    std::int32_t state = 2;
    EXPECT_EQ(sceNpGetState(1, &state), 0);
    EXPECT_EQ(state, 1);
}

TEST(HleNet, NpSizedRequestSucceeds) {
    // Laid out like the block built at 0x1fb6fc6..0x1fb7047; the field values are arbitrary.
    struct Block {
        std::uint64_t size;
        std::uint8_t byte;
        std::int32_t value;
        std::uint64_t qword;
    };
    static_assert(sizeof(Block) == 0x18);
    static_assert(offsetof(Block, byte) == 0x8);
    static_assert(offsetof(Block, value) == 0xc);
    static_assert(offsetof(Block, qword) == 0x10);
    Block block{0x18, 2, 3, 0};
    EXPECT_EQ(np_manager_sized_request(&block), 0);
    block.size = 0x20;
    bb::common::set_thread_name("np-test");
    testing::internal::CaptureStderr();
    EXPECT_EQ(np_manager_sized_request(&block), 0);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(std::regex_match(
        log, std::regex("[0-9:.]{12} warn \\[np-test\\] hle\\.libSceNpManager: A2CQ3kgSopQ: "
                        "block size 0x20, expected 0x18\n")))
        << log;
}

// The eboot imports each of them under the library it is registered for, so relocating it puts
// the implementation into the GOT slot.
TEST(HleNetGame, EbootImportsBindToTheImplementations) {
    BB_REQUIRE_GAME(game);
    bb::loader::Image image = bb::loader::map_eboot(game);
    bb::loader::relocate(image, bb::hle::resolve, bb::loader::eboot_module_id);

    struct Expected {
        std::string_view library, nid;
        void* impl;
    };
    const std::array<Expected, 12> expected{{
        {"libSceNet", "Nlev7Lg8k3A", reinterpret_cast<void*>(&sceNetInit)},
        {"libSceNet", "dgJBaeJnGpo", reinterpret_cast<void*>(&sceNetPoolCreate)},
        {"libSceSsl", "hdpVEUDFW3s", reinterpret_cast<void*>(&sceSslInit)},
        {"libSceHttp", "A9cVMUtEp4Y", reinterpret_cast<void*>(&sceHttpInit)},
        {"libSceHttp", "0gYjPTR-6cY", reinterpret_cast<void*>(&sceHttpCreateTemplate)},
        {"libSceHttp", "6381dWF+xsQ", reinterpret_cast<void*>(&sceHttpCreateEpoll)},
        {"libSceHttp", "s2-NPIvz+iA", reinterpret_cast<void*>(&http_template_set_int)},
        {"libSceHttp", "EY28T2bkN7k", reinterpret_cast<void*>(&http_template_set_string_pair)},
        {"libSceNetCtl", "uBPlr0lbuiI", reinterpret_cast<void*>(&sceNetCtlGetState)},
        {"libSceNetCtl", "obuxdTiwkF8", reinterpret_cast<void*>(&sceNetCtlGetInfo)},
        {"libSceNpManager", "eQH7nWPcAgc", reinterpret_cast<void*>(&sceNpGetState)},
        {"libSceNpManager", "A2CQ3kgSopQ", reinterpret_cast<void*>(&np_manager_sized_request)},
    }};
    std::set<std::string> imports;
    for (const bb::loader::Symbol& sym : image.dyn.symbols) {
        if (sym.kind == bb::loader::Symbol::Kind::import) {
            imports.insert(bb::hle::import_key(sym.decoded.library, sym.decoded.nid));
        }
    }
    for (const Expected& e : expected) {
        SCOPED_TRACE(e.nid);
        // import_address aborts for an import that was never resolved.
        ASSERT_TRUE(imports.contains(bb::hle::import_key(e.library, e.nid)));
        EXPECT_EQ(bb::hle::import_address(e.library, e.nid),
                  reinterpret_cast<std::uintptr_t>(e.impl));
    }
}

}  // namespace
