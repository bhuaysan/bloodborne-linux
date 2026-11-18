// The frame buffer size globals of src/game/gfx/frame_buffer.cpp: they hold the game's 1920x1080
// in its data, lie in a writable segment, and set_frame_buffer_size writes both (#515). The heap
// size table holds the GFX_GraphicsPrivate size that set_graphics_heap_size grows (#640).
#include "game/gfx/frame_buffer.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/eboot.hpp"

namespace {

using bb::game::frame_buffer_height;
using bb::game::graphics_private_heap_default;
using bb::game::graphics_private_heap_size;
using bb::game::graphics_private_heap_size_entry;
using bb::game::heap_size_table;
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

// The size of heap `heap` (the order of the names at 0x05358cc0) in configuration row `row`.
std::uint64_t table_size(int row, int heap) {
    return bb::game::global<std::uint64_t>(heap_size_table + row * 0x80 + heap * 8);
}

TEST_F(EbootTest, HeapSizeTableHoldsTheGraphicsHeaps) {
    // Read-only data: the write needs the page made writable.
    EXPECT_EQ(image().segment_at(heap_size_table, 9 * 0x80, PROT_WRITE), nullptr);
    EXPECT_NE(image().segment_at(heap_size_table, 9 * 0x80, PROT_READ), nullptr);
    // Master_ps4_GFXMIX (row 1): MAIN, GFX, GFX_SystemShared, GFX_GraphicsPrivate and
    // GFX_GraphicsPrivateB.
    EXPECT_EQ(table_size(1, 0), 0x20c00000U);
    EXPECT_EQ(table_size(1, 1), 0x11400000U);
    EXPECT_EQ(table_size(1, 2), 0x09800000U);
    EXPECT_EQ(table_size(1, 3), graphics_private_heap_default);
    EXPECT_EQ(table_size(1, 4), 0x37600000U);
    EXPECT_EQ(heap_size_table + 0x80 + 3 * 8, graphics_private_heap_size_entry);
    // Master_ps4_flexible (row 2), which the heap setup reads first, has no GFX_GraphicsPrivate.
    EXPECT_EQ(table_size(2, 3), 0U);
    // The rows and the column by the names the heap setup compares: the configuration names at
    // 0x05358c70 and the heap names at 0x05358cc0, pointers to UTF-16 strings.
    const auto name = [](std::uint64_t pointer_link) {
        return std::u16string_view(
            reinterpret_cast<const char16_t*>(bb::game::global<std::uintptr_t>(pointer_link)));
    };
    EXPECT_EQ(name(0x05358c70 + 1 * 8), u"Master_ps4_GFXMIX");
    EXPECT_EQ(name(0x05358c70 + 2 * 8), u"Master_ps4_flexible");
    EXPECT_EQ(name(0x05358cc0 + 3 * 8), u"GFX_GraphicsPrivate");
    EXPECT_EQ(name(0x05358cc0 + 4 * 8), u"GFX_GraphicsPrivateB");
}

TEST(GraphicsHeapSize, GrowsWithThePixelsAbove1080p) {
    constexpr std::uint64_t mib = 1 << 20;
    EXPECT_EQ(graphics_private_heap_size(1920, 1080), graphics_private_heap_default);
    EXPECT_EQ(graphics_private_heap_size(1280, 720), graphics_private_heap_default);
    // 1612800 more pixels at 288 bytes: 464486400 bytes, 443.0 MiB rounded up.
    EXPECT_EQ(graphics_private_heap_size(2560, 1440), graphics_private_heap_default + 443 * mib);
    // 6220800 more pixels: 1791590400 bytes, 1708.6 MiB rounded up.
    EXPECT_EQ(graphics_private_heap_size(3840, 2160), graphics_private_heap_default + 1709 * mib);
    // A whole number of MiB, also when the growth is not.
    EXPECT_EQ(graphics_private_heap_size(1984, 1116) % mib, 0U);
    EXPECT_GT(graphics_private_heap_size(1984, 1116), graphics_private_heap_default);
}

// The permissions ("r-xp" and the like) of the mapping in /proc/self/maps that holds `address`.
std::string mapping_permissions(std::uintptr_t address) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream in(line);
        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        char dash = 0;
        std::string permissions;
        in >> std::hex >> start >> dash >> end >> permissions;
        if (start <= address && address < end) {
            return permissions;
        }
    }
    return "";
}

TEST_F(EbootTest, SetGraphicsHeapSizeWritesTheTableEntry) {
    const std::uintptr_t entry = image().runtime(graphics_private_heap_size_entry);
    const std::string before = mapping_permissions(entry);
    EXPECT_EQ(before.substr(0, 3), "r-x");
    bb::game::set_graphics_heap_size(image(), 3840, 2160);
    EXPECT_EQ(table_size(1, 3), graphics_private_heap_size(3840, 2160));
    EXPECT_EQ(table_size(1, 4), 0x37600000U);
    EXPECT_EQ(table_size(1, 2), 0x09800000U);
    // The page gets its protection back: the game's read-only data stays read-only.
    EXPECT_EQ(mapping_permissions(entry), before);
    bb::game::set_graphics_heap_size(image(), 1920, 1080);
    EXPECT_EQ(table_size(1, 3), graphics_private_heap_default);
    EXPECT_EQ(mapping_permissions(entry), before);
}

}  // namespace
