// Dynamic rendering into the colour and depth targets of the register mirror, with the clears
// that register state asks for, and the fast-clear eliminate pass (docs/design/graphics.md,
// "Render targets and depth").
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include <vulkan/vulkan.h>

#include "gpu/pm4/registers.hpp"
#include "gpu/pm4/targets.hpp"

namespace bb::vk {

class Device;
class ImageCache;
struct Image;

// What a pipeline must match to draw inside the rendering that begin() records
// (VkPipelineRenderingCreateInfo and the sample count), and the rendering's size.
struct RenderingInfo {
    std::array<VkFormat, pm4::color_targets> color{};  // VK_FORMAT_UNDEFINED in unbound slots
    std::uint32_t color_count = 0;                     // the last bound slot + 1
    VkFormat depth = VK_FORMAT_UNDEFINED;    // the depth attachment's format, if there is one
    VkFormat stencil = VK_FORMAT_UNDEFINED;  // the stencil attachment's format, if there is one
    std::uint32_t samples = 1;
    VkExtent2D extent{};      // the smallest extent of the attachments
    std::uint32_t layers = 0;  // the fewest view layers of the attachments
};

// Records the start of rendering into the targets of the register mirror, with their images
// from the image cache, and the clears of register state. Keeps the image views it makes.
class TargetRenderer {
public:
    // The device and the cache must outlive the renderer, and the GPU must be done with its
    // views when it is destroyed.
    TargetRenderer(const Device& device, ImageCache& images) : device_(device), images_(images) {}
    ~TargetRenderer();
    TargetRenderer(const TargetRenderer&) = delete;
    TargetRenderer& operator=(const TargetRenderer&) = delete;

    // Marks the CMASK or HTILE at `address` as cleared, as a compute shader or fill that writes
    // it does (the dispatch and memory items report that). The next begin() or
    // eliminate_fast_clear() that binds a target with this metadata clears the target's view
    // and takes the mark.
    void mark_cleared(std::uint64_t meta_address) { cleared_.insert(meta_address); }
    bool cleared(std::uint64_t meta_address) const { return cleared_.contains(meta_address); }

    // Begins dynamic rendering into the bound targets and returns what the pipeline must
    // match; nothing, with nothing recorded, when no target is bound.
    // - Colour target n is bound with CB_COLOR_CONTROL.MODE normal (1) and a non-zero nibble n
    //   of CB_TARGET_MASK. When its CMASK was marked cleared, its whole view is cleared to its
    //   clear words before the rendering; the rendering loads it.
    // - The depth target is bound when DB_DEPTH_CONTROL enables the depth test for a depth part
    //   or the stencil test for a stencil part. Depth is cleared at load to DB_DEPTH_CLEAR
    //   (clamped to 0-1) with DB_RENDER_CONTROL.DEPTH_CLEAR_ENABLE, the depth test and depth
    //   writes, and its whole view before the rendering when its HTILE was marked cleared.
    //   Stencil is cleared at load to DB_STENCIL_CLEAR with STENCIL_CLEAR_ENABLE.
    // - An attachment whose sample count differs from the first one's is left out (logged).
    // Each view covers the target's slices base_slice to slices - 1. The images move to the
    // attachment layouts behind a barrier from all earlier commands, the clears included.
    std::optional<RenderingInfo> begin(VkCommandBuffer commands, const pm4::RegisterMirror& regs,
                                       const pm4::ExtentHints& hints);
    // Ends the rendering that begin() began.
    static void end(VkCommandBuffer commands) { vkCmdEndRendering(commands); }

    // The fast-clear eliminate pass (CB_COLOR_CONTROL.MODE 2): when colour target 0 has
    // FAST_CLEAR and its CMASK was marked cleared, clears its view to its clear words, leaving
    // the image in TRANSFER_DST_OPTIMAL, and takes the mark. Returns whether it recorded the
    // clear.
    bool eliminate_fast_clear(VkCommandBuffer commands, const pm4::RegisterMirror& regs,
                              const pm4::ExtentHints& hints);

private:
    VkImageView view(const Image& image, std::uint32_t base_slice, std::uint32_t layers);
    bool take_cleared(std::uint64_t meta_address) { return cleared_.erase(meta_address) != 0; }

    const Device& device_;
    ImageCache& images_;
    std::set<std::uint64_t> cleared_;
    std::map<std::tuple<VkImage, std::uint32_t, std::uint32_t>, VkImageView> views_;
};

}  // namespace bb::vk
