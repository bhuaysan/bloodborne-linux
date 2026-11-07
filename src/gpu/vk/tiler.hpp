// The compute detiler and tiler (docs/design/graphics.md, "Tiling (item 23, as built)"): the
// shader that copies a surface between tiled guest memory and its linear copy on the GPU, one
// invocation per element, with the address tables of bb::tile::address_tables.
#pragma once

#include <cstdint>
#include <vector>

namespace bb::vk {

// The push constants of the tiling shader: device addresses of one level in the tiled and the
// linear copy and of its tables (columns, then rows, then layers, 8 bytes per entry), the
// level's padded size, and the constants of bb::tile::AddressTables.
struct TilingConstants {
    std::uint64_t tiled = 0;
    std::uint64_t linear = 0;
    std::uint64_t tables = 0;
    std::uint32_t pitch = 0;
    std::uint32_t height = 0;
    std::uint32_t split_bytes = 0;
    std::uint32_t split_rotation = 0;
    std::uint32_t pipe_bank_bits = 0;
    std::uint32_t pipe_bank_mask = 0;
};
static_assert(sizeof(TilingConstants) == 48);

// The tiling shader for elements of `bytes` bytes (1, 2, 4, 8 or 16): an invocation (x, y, z)
// of a dispatch over pitch by height by layers copies element (x, y) of layer z from the tiled
// to the linear copy (`to_linear`) or back. Workgroups are 8x8x1; invocations past the pitch
// or the height do nothing. Throws std::invalid_argument for another size.
std::vector<std::uint32_t> tiling_shader(std::uint32_t bytes, bool to_linear);

}  // namespace bb::vk
