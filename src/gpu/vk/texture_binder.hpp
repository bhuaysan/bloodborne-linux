// Binds the textures and samplers of a draw (docs/design/graphics.md, "Textures and samplers
// (item 28)"): finds the T# and S# of a stage's interface in its user data and guest memory, makes
// the images and samplers through the TextureCache, and fills one descriptor set per stage.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu/gcn/translate.hpp"
#include "gpu/pm4/registers.hpp"
#include "gpu/vk/descriptor_layouts.hpp"
#include "gpu/vk/texture_cache.hpp"

namespace bb::vk {

class Device;
class Scheduler;

// The guest bytes [address, address + bytes), or none when they aren't all readable.
using GuestReader = std::function<std::span<const std::byte>(std::uint64_t address,
                                                             std::uint64_t bytes)>;

// Used by one thread (the GPU thread). The device, the scheduler and the cache must outlive it.
class TextureBinder {
public:
    TextureBinder(const Device& device, Scheduler& scheduler, TextureCache& textures,
                  GuestReader read);
    ~TextureBinder();
    TextureBinder(const TextureBinder&) = delete;
    TextureBinder& operator=(const TextureBinder&) = delete;

    // The image class of each T# of `interface`, read from the stage's user data and guest
    // memory now (compare them with the ShaderKey the program was translated with). Nothing, with
    // `why` saying what, when a T# can't be found or read.
    std::optional<std::vector<gcn::ImageClass>> classes(
        const gcn::ShaderInterface& interface,
        std::span<const std::uint32_t, pm4::user_data_dwords> user, std::string& why);

    // A descriptor set (layout from DescriptorLayouts for the interface) with the interface's
    // images and samplers, valid until the work recorded so far completes. The textures are
    // uploaded by commands recorded into the scheduler's current command buffer when new or
    // changed since the last submission began (once per submission), so call it outside a render
    // pass. A null set, with `why`, when a descriptor can't be read, a texture or sampler can't
    // be made, or the program writes an image (not yet). Throws std::runtime_error when a Vulkan
    // call fails.
    VkDescriptorSet bind(const gcn::ShaderInterface& interface,
                         std::span<const std::uint32_t, pm4::user_data_dwords> user,
                         std::string& why);

    std::size_t pools() const { return pools_.size(); }  // for tests

private:
    struct Pool {
        VkDescriptorPool handle = VK_NULL_HANDLE;
        std::uint32_t pending = 0;    // submissions with sets from it that haven't completed
        std::uint64_t noted = 0;      // submitted() + 1 when the last one was counted
    };

    // `count` dwords of the descriptor at `source`.
    bool read(const gcn::ResourceSource& source,
              std::span<const std::uint32_t, pm4::user_data_dwords> user, std::size_t count,
              std::array<std::uint32_t, 8>& out, std::string& why);
    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    Pool& next_pool();

    const Device& device_;
    Scheduler& scheduler_;
    TextureCache& textures_;
    GuestReader read_guest_;
    DescriptorLayouts layouts_;
    std::uint64_t submission_ = ~std::uint64_t{0};  // scheduler submitted() at the last bind
    std::deque<Pool> pools_;  // nodes stay where they are: deferred functions point at them
    Pool* current_ = nullptr;
};

}  // namespace bb::vk
