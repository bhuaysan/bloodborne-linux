// The frame buffer size globals of src/game/gfx/frame_buffer.cpp: they hold the game's 1920x1080
// in its data, lie in a writable segment, and set_frame_buffer_size writes both (#515).
#include "game/gfx/frame_buffer.hpp"

#include <sys/mman.h>

#include <cstdint>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/eboot.hpp"

namespace {

using bb::game::frame_buffer_height;
using bb::game::frame_buffer_width;
using bb::test::EbootTest;

TEST_F(EbootTest, FrameBufferSizeIsWritableDataOf1080p) {
    EXPECT_NE(image().segment_at(frame_buffer_width, 8, PROT_READ | PROT_WRITE), nullptr);
    EXPECT_EQ(bb::game::global<std::uint32_t>(frame_buffer_width), 1920U);
    EXPECT_EQ(bb::game::global<std::uint32_t>(frame_buffer_height), 1080U);
}

TEST_F(EbootTest, SetFrameBufferSizeWritesBothGlobals) {
    const std::uint32_t old_width = bb::game::global<std::uint32_t>(frame_buffer_width);
    const std::uint32_t old_height = bb::game::global<std::uint32_t>(frame_buffer_height);
    bb::game::set_frame_buffer_size(2560, 1440);
    EXPECT_EQ(bb::game::global<std::uint32_t>(frame_buffer_width), 2560U);
    EXPECT_EQ(bb::game::global<std::uint32_t>(frame_buffer_height), 1440U);
    bb::game::set_frame_buffer_size(old_width, old_height);
    EXPECT_EQ(bb::game::global<std::uint32_t>(frame_buffer_width), old_width);
    EXPECT_EQ(bb::game::global<std::uint32_t>(frame_buffer_height), old_height);
}

}  // namespace
