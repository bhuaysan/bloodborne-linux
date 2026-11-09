#include "gpu/vk/texture_binder.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>

#include "common/log.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/scheduler.hpp"
#include "gpu/vk/texture_desc.hpp"

namespace bb::vk {
namespace {

constexpr std::uint32_t pool_sets = 128, pool_images = 64, pool_samplers = 64;  // per set
constexpr std::uint64_t address_mask = (std::uint64_t{1} << 48) - 1;

}  // namespace

TextureBinder::TextureBinder(const Device& device, Scheduler& scheduler, TextureCache& textures,
                             GuestReader read)
    : device_(device), scheduler_(scheduler), textures_(textures), read_guest_(std::move(read)),
      layouts_(device) {}

TextureBinder::~TextureBinder() {
    scheduler_.finish();  // runs the deferred functions that point at the pools
    for (const Pool& pool : pools_) {
        vkDestroyDescriptorPool(device_.handle(), pool.handle, nullptr);
    }
}

bool TextureBinder::read(const gcn::ResourceSource& source,
                         std::span<const std::uint32_t, pm4::user_data_dwords> user,
                         std::size_t count, std::array<std::uint32_t, 8>& out, std::string& why) {
    const auto& path = source.path;
    if (path.empty() || path[0] + (path.size() == 1 ? count : 2) > user.size()) {
        why = "a descriptor's user data slot is outside the user data";
        return false;
    }
    if (path.size() == 1) {
        std::copy_n(user.begin() + path[0], count, out.begin());
        return true;
    }
    // A pointer in two dwords; every further entry is the dword offset of the next pointer, or,
    // for the last, of the descriptor.
    std::uint64_t address = (std::uint64_t{user[path[0] + 1]} << 32 | user[path[0]]) & address_mask;
    for (std::size_t k = 1; k < path.size(); ++k) {
        const bool last = k + 1 == path.size();
        const std::uint64_t at = address + std::uint64_t{path[k]} * 4;
        const std::uint64_t bytes = last ? count * 4 : 8;
        const auto guest = read_guest_(at, bytes);
        if (guest.size() < bytes) {
            why = std::format("the descriptor memory at {:#x} isn't readable", at);
            return false;
        }
        if (last) {
            std::memcpy(out.data(), guest.data(), bytes);
        } else {
            std::uint64_t pointer;
            std::memcpy(&pointer, guest.data(), 8);
            address = pointer & address_mask;
        }
    }
    return true;
}

std::optional<std::vector<gcn::ImageClass>> TextureBinder::classes(
    const gcn::ShaderInterface& interface,
    std::span<const std::uint32_t, pm4::user_data_dwords> user, std::string& why) {
    std::vector<gcn::ImageClass> out;
    std::array<std::uint32_t, 8> dwords{};
    for (const gcn::ResourceSource& source : interface.images) {
        if (!read(source, user, 4, dwords, why)) {
            return std::nullopt;
        }
        out.push_back(gcn::image_class(std::span<const std::uint32_t, 4>(dwords.data(), 4)));
    }
    return out;
}

VkDescriptorSet TextureBinder::bind(const gcn::ShaderInterface& interface,
                                    std::span<const std::uint32_t, pm4::user_data_dwords> user,
                                    std::string& why) {
    if (std::ranges::any_of(interface.stored_images, [](bool stored) { return stored; })) {
        why = "images written by a shader aren't supported yet";
        return VK_NULL_HANDLE;
    }
    // Textures are checked against guest memory once per submission.
    if (scheduler_.submitted() != submission_) {
        submission_ = scheduler_.submitted();
        textures_.begin_submission();
    }
    std::vector<VkDescriptorImageInfo> infos;
    infos.reserve(interface.images.size() + interface.samplers.size());
    SamplerLimits limits;
    std::array<std::uint32_t, 8> dwords{};
    for (const gcn::ResourceSource& source : interface.images) {
        if (!read(source, user, 8, dwords, why)) {
            return VK_NULL_HANDLE;
        }
        const auto desc = decode_texture(dwords);
        if (!desc) {
            why = "an image descriptor isn't a T#";
            return VK_NULL_HANDLE;
        }
        const auto layout = TextureCache::guest_layout(*desc);
        if (!layout) {
            why = std::format("the texture at {:#x} can't be made (type {}, format {}/{}, tile "
                              "mode {})",
                              desc->address, desc->type, desc->data_format, desc->num_format,
                              desc->tile_mode);
            return VK_NULL_HANDLE;
        }
        const auto guest = read_guest_(desc->address, layout->size);
        if (guest.size() < layout->size) {
            why = std::format("the texture at {:#x} isn't readable", desc->address);
            return VK_NULL_HANDLE;
        }
        const TextureView* view = textures_.get(*desc, guest);
        if (view == nullptr) {
            why = std::format("the texture at {:#x} can't be made", desc->address);
            return VK_NULL_HANDLE;
        }
        limits.linear &= view->filter_linear;
        limits.integer |= view->integer;
        infos.push_back({VK_NULL_HANDLE, view->view, TextureView::layout});
    }
    for (const gcn::ResourceSource& source : interface.samplers) {
        if (!read(source, user, 4, dwords, why)) {
            return VK_NULL_HANDLE;
        }
        const SamplerDesc desc = decode_sampler(std::span<const std::uint32_t, 4>(dwords.data(), 4));
        if (const auto unsupported = unsupported_sampler(desc)) {
            why = *unsupported;
            return VK_NULL_HANDLE;
        }
        const VkSampler sampler = textures_.sampler(desc, false, limits);
        if (sampler == VK_NULL_HANDLE) {
            why = "a sampler can't be made";
            return VK_NULL_HANDLE;
        }
        infos.push_back({sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    }
    const VkDescriptorSet set = allocate(layouts_.get(interface));
    std::vector<VkWriteDescriptorSet> writes;
    for (std::uint32_t i = 0; i < infos.size(); ++i) {
        auto write = make<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        write.dstSet = set;
        write.dstBinding = i;
        write.descriptorCount = 1;
        write.descriptorType = i < interface.images.size() ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                           : VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &infos[i];
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(device_.handle(), static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    return set;
}

VkDescriptorSet TextureBinder::allocate(VkDescriptorSetLayout layout) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (current_ == nullptr) {
            current_ = &next_pool();
        }
        auto info = make<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        info.descriptorPool = current_->handle;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = vkAllocateDescriptorSets(device_.handle(), &info, &set);
        if (result == VK_SUCCESS) {
            // The pool's sets are in use until the work recorded now completes: count each
            // submission once, and reset the pool when the last one has.
            Pool& pool = *current_;
            if (pool.noted != scheduler_.submitted() + 1) {
                pool.noted = scheduler_.submitted() + 1;
                ++pool.pending;
                scheduler_.defer([this, &pool] {
                    if (--pool.pending == 0) {
                        vkResetDescriptorPool(device_.handle(), pool.handle, 0);
                    }
                });
            }
            return set;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            check(result, "vkAllocateDescriptorSets");
        }
        current_ = &next_pool();  // full: another pool, once
    }
    throw std::runtime_error("a descriptor set doesn't fit in an empty pool");
}

TextureBinder::Pool& TextureBinder::next_pool() {
    for (Pool& pool : pools_) {
        if (&pool != current_ && pool.pending == 0) {
            return pool;  // reset when its last submission completed
        }
    }
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, pool_sets * pool_images},
        {VK_DESCRIPTOR_TYPE_SAMPLER, pool_sets * pool_samplers}};
    auto info = make<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    info.maxSets = pool_sets;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    VkDescriptorPool handle = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(device_.handle(), &info, nullptr, &handle),
          "vkCreateDescriptorPool");
    pools_.push_back({handle});
    return pools_.back();
}

}  // namespace bb::vk
