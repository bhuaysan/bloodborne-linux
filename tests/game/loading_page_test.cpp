// Tests of the loading page backdrop (src/game/gfx/loading_page.cpp) and the device clear colour
// (src/game/gfx/frame_buffer.cpp) at a frame buffer wider than 16:9 (#711): which movie gets the
// backdrop, which colour each size clears to, and, against the eboot, the code both rely on.
#include "game/gfx/loading_page.hpp"

#include <sys/mman.h>

#include <array>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "game/camera/aspect.hpp"
#include "game/gfx/frame_buffer.hpp"
#include "game/image.hpp"
#include "harness/eboot.hpp"

namespace {

using bb::game::device_clear_black;
using bb::game::device_clear_color;
using bb::game::device_clear_color_for;
using bb::game::device_clear_grey;
using bb::game::game_aspect;
using bb::game::wants_black_backdrop;
using bb::test::EbootTest;

TEST(LoadingPage, OnlyTheLoadingPageAtAWideSizeGetsTheBackdrop) {
    const float wide = 64.0f / 27.0f;
    EXPECT_TRUE(wants_black_backdrop(u"NowLoading2", wide));
    EXPECT_TRUE(wants_black_backdrop(u"NowLoading2", 32.0f / 9.0f));
    EXPECT_FALSE(wants_black_backdrop(u"NowLoading2", game_aspect));
    // The HUD, the menus and the fade keep showing what is under them.
    EXPECT_FALSE(wants_black_backdrop(u"Black", wide));
    EXPECT_FALSE(wants_black_backdrop(u"MessageBox", wide));
    EXPECT_FALSE(wants_black_backdrop(u"Title_DLC_EU", wide));
    EXPECT_FALSE(wants_black_backdrop(u"NowLoading", wide));
    EXPECT_FALSE(wants_black_backdrop(u"NowLoading23", wide));
    EXPECT_FALSE(wants_black_backdrop(u"", wide));
}

TEST(DeviceClearColor, GreyAt16x9BlackOtherwise) {
    EXPECT_EQ(device_clear_color_for(1920, 1080), device_clear_grey);
    EXPECT_EQ(device_clear_color_for(2560, 1440), device_clear_grey);
    EXPECT_EQ(device_clear_color_for(3840, 2160), device_clear_grey);
    EXPECT_EQ(device_clear_color_for(2560, 1080), device_clear_black);
    EXPECT_EQ(device_clear_color_for(3440, 1440), device_clear_black);
    EXPECT_EQ(device_clear_color_for(5120, 1440), device_clear_black);
}

using LoadingPageGame = EbootTest;

const std::uint8_t* at(std::uint64_t link) {
    return reinterpret_cast<const std::uint8_t*>(bb::game::image().runtime(link));
}

TEST_F(LoadingPageGame, TheMovieHolderSetsATransparentBackground) {
    EXPECT_EQ(std::memcmp(at(bb::game::movie_holder_construct),
                          bb::game::movie_holder_construct_prologue.data(),
                          bb::game::movie_holder_construct_prologue.size()),
              0);
    // mov rax, [rax+0x108]; mov dword [rbp-0x90], 0; lea rsi, [rbp-0x90]: the background
    // colour 0 by pointer through the movie's vtable slot.
    constexpr std::array<std::uint8_t, 24> set_background = {
        0x48, 0x8b, 0x80, 0x08, 0x01, 0x00, 0x00, 0xc7, 0x85, 0x70, 0xff, 0xff,
        0xff, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0xb5, 0x70, 0xff, 0xff, 0xff};
    EXPECT_EQ(std::memcmp(at(0x01f584fb), set_background.data(), set_background.size()), 0);
    std::uint32_t slot = 0;
    std::memcpy(&slot, at(0x01f584fe), sizeof slot);
    EXPECT_EQ(slot, bb::game::movie_set_background_slot);
}

TEST_F(LoadingPageGame, GraphicsInitReadsTheGreyClearColour) {
    std::array<float, 4> color{};
    std::memcpy(color.data(), at(device_clear_color), sizeof color);
    EXPECT_EQ(color, device_clear_grey);
    // vmovaps xmm0, [rip + rel32] at 0x01d96afb, 8 bytes long, reads the colour.
    constexpr std::array<std::uint8_t, 4> vmovaps = {0xc5, 0xf8, 0x28, 0x05};
    EXPECT_EQ(std::memcmp(at(0x01d96afb), vmovaps.data(), vmovaps.size()), 0);
    std::int32_t rel = 0;
    std::memcpy(&rel, at(0x01d96aff), sizeof rel);
    EXPECT_EQ(0x01d96b03 + rel, static_cast<std::int64_t>(device_clear_color));
}

TEST_F(LoadingPageGame, SetDeviceClearColorWritesAndRestores) {
    bb::game::set_device_clear_color(image(), 2560, 1080);
    std::array<float, 4> color{};
    std::memcpy(color.data(), at(device_clear_color), sizeof color);
    EXPECT_EQ(color, device_clear_black);
    bb::game::set_device_clear_color(image(), 1920, 1080);
    std::memcpy(color.data(), at(device_clear_color), sizeof color);
    EXPECT_EQ(color, device_clear_grey);
    // The colour lies in the read-only segment, and the write leaves it read-only.
    EXPECT_EQ(image().segment_at(device_clear_color, 16, PROT_READ | PROT_WRITE), nullptr);
}

}  // namespace
