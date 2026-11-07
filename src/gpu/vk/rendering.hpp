// Dynamic rendering into the colour targets of the register mirror, with the fast clears that
// register state asks for (docs/design/graphics.md, "Render targets and depth"). The depth
// target and the eliminate pass come next.
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

    // Marks the CMASK at `address` as cleared, as a compute shader or fill that writes it does
    // (the dispatch and memory items report that). The next begin() that binds a target with
    // this CMASK clears the target and takes the mark.
    void mark_cleared(std::uint64_t meta_address) { cleared_.insert(meta_address); }
    bool cleared(std::uint64_t meta_address) const { return cleared_.contains(meta_address); }

    // Begins dynamic rendering into the bound targets and returns what the pipeline must
    // match; nothing, with nothing recorded, when no target is bound.
    // Colour target n is bound with CB_COLOR_CONTROL.MODE normal (1) and a non-zero nibble n of
    // CB_TARGET_MASK. It is cleared to its clear words when its CMASK was marked cleared,
    // otherwise loaded. A target whose sample count differs from the first one's is left out
    // (logged). Each view covers the target's slices base_slice to slices - 1. The images move
    // to the attachment layout behind a barrier from all earlier commands.
    std::optional<RenderingInfo> begin(VkCommandBuffer commands, const pm4::RegisterMirror& regs,
                                       const pm4::ExtentHints& hints);
    // Ends the rendering that begin() began.
    static void end(VkCommandBuffer commands) { vkCmdEndRendering(commands); }

private:
    VkImageView view(const Image& image, std::uint32_t base_slice, std::uint32_t layers);
    bool take_cleared(std::uint64_t meta_address) { return cleared_.erase(meta_address) != 0; }

    const Device& device_;
    ImageCache& images_;
    std::set<std::uint64_t> cleared_;
    std::map<std::tuple<VkImage, std::uint32_t, std::uint32_t>, VkImageView> views_;
};

}  // namespace bb::vk
