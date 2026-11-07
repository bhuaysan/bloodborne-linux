// The compute detiler and tiler (docs/design/graphics.md, "Tiling (item 23, as built)"): the
// shader that copies a surface between tiled guest memory and its linear copy on the GPU, one
// invocation per element, with the address tables of bb::tile::address_tables.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu/tile/tiling.hpp"

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

// The tables of bb::tile::address_tables as the shader reads them: the column entries, then the
// row entries, then the layer entries, two words each.
std::vector<std::uint32_t> table_words(const tile::AddressTables& tables);

class Device;

// The tiling pipelines of one device, made on first use.
class Tiler {
public:
    // Throws std::runtime_error when a Vulkan call fails. The device must outlive the tiler.
    explicit Tiler(const Device& device);
    ~Tiler();
    Tiler(const Tiler&) = delete;
    Tiler& operator=(const Tiler&) = delete;

    // Device addresses of the surface's base in tiled guest memory and in its linear copy (each
    // layout.size bytes), and of table_words(tables) for the level.
    struct Addresses {
        VkDeviceAddress tiled = 0, linear = 0, tables = 0;
    };

    // Records into `commands` a dispatch that copies every element of every layer of `level`
    // from the tiled to the linear copy (`to_linear`) or back, with `tables` =
    // address_tables(layout, level). The caller orders it against other work with barriers.
    // Throws std::runtime_error when a Vulkan call fails.
    void record(VkCommandBuffer commands, const tile::Layout& layout, std::uint32_t level,
                const tile::AddressTables& tables, const Addresses& at, bool to_linear);

private:
    VkDevice device_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    // By log2 of the element size, then to_linear.
    std::array<std::array<VkPipeline, 2>, 5> pipelines_{};
};

}  // namespace bb::vk
