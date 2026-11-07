// The Vulkan formats of colour and depth targets (docs/design/graphics.md, "Render targets and
// depth").
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace bb::vk {

// The format of a colour target with CB_COLOR<n>_INFO's FORMAT, NUMBER_TYPE and COMP_SWAP. Its
// memory layout is the target's, so an image can be copied to and from guest memory as it is.
// COMP_SWAP 1 (alternate) turns 8_8_8_8 UNORM and SRGB into B8G8R8A8; other swaps keep the
// format and leave the order of the components to the pipeline. VK_FORMAT_UNDEFINED for a
// format or number type that a target can't have.
VkFormat color_format(std::uint32_t format, std::uint32_t number_type, std::uint32_t comp_swap);

// The format of a depth target with DB_Z_INFO.FORMAT (0 none, 1 Z16, 3 Z32_FLOAT) and with or
// without an 8-bit stencil. VK_FORMAT_UNDEFINED for other depth formats or neither part.
VkFormat depth_format(std::uint32_t z_format, bool stencil);

}  // namespace bb::vk
