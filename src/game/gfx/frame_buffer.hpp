// The engine's frame buffer size, which bb-run --resolution sets before the game starts
// (docs/design/resolution.md, "Decision: way B").
#pragma once

#include <cstdint>

#include "loader/image.hpp"

namespace bb::game {

// The engine sizes every screen-sized render target, the post-processing chain, the compute grids,
// the viewports and the display buffers from these two globals. Their data defaults are 1920 and
// 1080; system_properties_init (0x02017770) reads them as the fallbacks of its configuration keys
// and writes them back, and graphics_init (0x01d969f0) builds the graphics device from them.
inline constexpr std::uint64_t frame_buffer_width = 0x051289f8;
inline constexpr std::uint64_t frame_buffer_height = 0x051289fc;

// Writes `width` and `height` to the two globals of image(). Called once after the eboot is
// mapped and before its entry point runs, so every reader sees the new size. Unlike the game's
// own setter of the same name (0x01d9e850) it leaves the HUD scale factors (0x055404d0,
// 0x055404d4) alone: system_properties_init computes them from the globals later. Writing the
// globals once the game runs would leave those factors and the allocated targets at the old size.
void set_frame_buffer_size(std::uint32_t width, std::uint32_t height);

// The engine's heap sizes: SprjMemory's table at 0x04736d40, one row of 16 sizes in bytes for each
// of 9 build configurations, the heaps in the order of the names at 0x05358cc0 (MAIN, GFX,
// GFX_SystemShared, GFX_GraphicsPrivate, ...). The heap setup (0x01fc0e60, 0x01fc7870,
// 0x01fc7d20) takes a heap's size from the row of the configuration Master_ps4_flexible first and,
// when that is 0, from the row of Master_ps4_GFXMIX; both indices come from configuration names
// in data. GFX_GraphicsPrivate has no flexible size, so its size is the Master_ps4_GFXMIX entry.
inline constexpr std::uint64_t heap_size_table = 0x04736d40;
inline constexpr std::uint64_t graphics_private_heap_size_entry = heap_size_table + 1 * 0x80 + 3 * 8;
inline constexpr std::uint64_t graphics_private_heap_default = 0x8ef00000;  // 2287 MiB

// GFX_GraphicsPrivate and GFX_GraphicsPrivateB (886 MiB) form the mix allocator
// GFX_GraphicsPrivateMix (0x0569d448): blocks of 1 MiB and more go to GraphicsPrivate first,
// smaller ones to GraphicsPrivateB first, and each falls back to the other (0x01fbeb30). The
// render targets and the map's textures come from it. In the clinic at 1x the two keep at least
// 600 MiB free. At 2560x1440 the game takes 406 MiB more of them, at 3840x2160 1642 MiB more, so
// at 3840x2160 the map's textures no longer fit (#640). That is 264 and 277 bytes for each pixel
// above 1080p; the growth below is the larger with a margin of 4 %, so every size keeps at least
// the free space of 1x.
inline constexpr std::uint64_t graphics_heap_bytes_per_pixel = 288;

// The size of GFX_GraphicsPrivate for a frame buffer of `width` x `height`: the game's 2287 MiB at
// 1920x1080 and below, plus graphics_heap_bytes_per_pixel for each pixel above 1920x1080, rounded
// up to whole MiB like the table's other sizes.
constexpr std::uint64_t graphics_private_heap_size(std::uint32_t width, std::uint32_t height) {
    constexpr std::uint64_t base_pixels = 1920 * 1080;
    constexpr std::uint64_t mib = std::uint64_t{1} << 20;
    const std::uint64_t pixels = std::uint64_t{width} * height;
    if (pixels <= base_pixels) {
        return graphics_private_heap_default;
    }
    const std::uint64_t extra = (pixels - base_pixels) * graphics_heap_bytes_per_pixel;
    return graphics_private_heap_default + (extra + mib - 1) / mib * mib;
}

// The largest size bb-run takes, 3840x2160, gives 3996 MiB: below 4 GiB like the sizes measured,
// and with the other heaps (4478 MiB together) inside the 8 GiB of direct memory the kernel layer
// has (src/hle/kernel/dmem.hpp).
static_assert(graphics_private_heap_size(3840, 2160) < (std::uint64_t{1} << 32));
static_assert(graphics_private_heap_size(3840, 2160) - graphics_private_heap_default +
                  std::uint64_t{4478} * (1 << 20) <
              (std::uint64_t{8} << 30));

// Writes graphics_private_heap_size(width, height) into the heap size table of `eboot`. Called
// once after the eboot is mapped and before its entry point runs: the heaps are set up at start.
// The table lies in the read-only segment; on a finalized image the page is made writable for the
// write and gets its protection back. At 1920x1080 it writes the value the table already holds.
void set_graphics_heap_size(loader::Image& eboot, std::uint32_t width, std::uint32_t height);

}  // namespace bb::game
