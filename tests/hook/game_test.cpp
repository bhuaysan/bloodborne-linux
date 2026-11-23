// The hook's test against the eboot, in eboot_game_test (tests/CMakeLists.txt): it maps the ELF
// that support/eboot_file.hpp read once per process.

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>

#include <gtest/gtest.h>

#include "hook/hook.hpp"
#include "loader/image.hpp"
#include "support/eboot_facts.hpp"
#include "support/eboot_file.hpp"
#include "support/elf_builder.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::loader;
using bb::hook::HookError;
using bb::hook::Replacement;
using bb::test::protection;
namespace facts = bb::test::eboot_facts;

// Expects `f` to throw a HookError whose message contains `message`.
template <class F>
void expect_refusal(F f, std::string_view message) {
    try {
        f();
        ADD_FAILURE() << "no error, expected: " << message;
    } catch (const HookError& e) {
        EXPECT_NE(std::string_view(e.what()).find(message), std::string_view::npos) << e.what();
    }
}

using Patch = std::array<std::byte, bb::hook::patch_size>;

Patch bytes_at(const Image& image, std::uint64_t link) {
    Patch out;
    std::memcpy(out.data(), reinterpret_cast<const void*>(image.base + link), out.size());
    return out;
}

int replacement_a() {
    return 1001;
}

TEST(HookGame, PatchesAnEbootFunction) {
    BB_REQUIRE_GAME(game);
    Image image = map_elf(bb::test::eboot_file(game).elf, default_base);
    image.finalize_protection();
    const std::uint64_t hooked = facts::hookable_function.link_addr;
    Patch original = bytes_at(image, hooked);
    const std::uint8_t start[] = {0xc7, 0x47, 0x08, 0x01};
    ASSERT_EQ(std::memcmp(original.data(), start, sizeof start), 0);

    // The entry jumps straight to the replacement, so no game code runs.
    const Replacement hook[] = {{hooked, reinterpret_cast<void*>(&replacement_a), "a"}};
    bb::hook::apply(image, hook);
    EXPECT_EQ(image.fn<int()>(hooked)(), 1001);
    bb::hook::remove(image, hook);
    EXPECT_EQ(std::memcmp(bytes_at(image, hooked).data(), original.data(), original.size()), 0);
    EXPECT_EQ(protection(default_base + hooked), "r-xp");

    const std::uint64_t short_link = facts::short_function.link_addr;
    const Replacement too_short[] = {{short_link, reinterpret_cast<void*>(&replacement_a), "s"}};
    expect_refusal([&] { bb::hook::apply(image, too_short); },
                   std::format("hook s at {:#x}: the function is {} bytes, a hook needs 14",
                               short_link, facts::short_function.size));
}

}  // namespace
