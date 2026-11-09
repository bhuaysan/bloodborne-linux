// The texture and sampler cache on lavapipe: a tiled image in guest memory arrives in a
// Vulkan image, and is uploaded again only when it changed.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/scheduler.hpp"
#include "gpu/vk/texture_cache.hpp"
#include "test_device.hpp"

namespace {

using namespace bb::vk;

class VkTextureCache : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = bb::test::vk_test_device()}, why);
        if (!device_) {
            GTEST_SKIP() << "no Vulkan device: " << why;
        }
        scheduler_ = std::make_unique<Scheduler>(*device_);
        cache_ = std::make_unique<TextureCache>(*device_, *scheduler_);
    }
    void TearDown() override {
        cache_.reset();
        scheduler_.reset();
    }

    // The tight texels of `levels` levels of `layers` layers of an image, read back.
    std::vector<std::byte> read_back(const TextureView& view, std::uint32_t width,
                                     std::uint32_t height, std::uint32_t bytes,
                                     std::uint32_t layers, std::uint32_t levels) {
        std::vector<VkBufferImageCopy> regions;
        std::uint64_t size = 0;
        for (std::uint32_t i = 0; i < levels; ++i) {
            const std::uint32_t w = std::max(width >> i, 1u), h = std::max(height >> i, 1u);
            regions.push_back({size, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, layers}, {0, 0, 0},
                               {w, h, 1}});
            size += std::uint64_t{w} * h * bytes * layers;
        }
        const VkDevice d = device_->handle();
        auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer;
        check(vkCreateBuffer(d, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(d, buffer, &needs);
        auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = *device_->memory_type(
            needs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory;
        check(vkAllocateMemory(d, &alloc, nullptr, &memory), "vkAllocateMemory");
        check(vkBindBufferMemory(d, buffer, memory, 0), "vkBindBufferMemory");
        void* map;
        check(vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory");
        vkCmdCopyImageToBuffer(scheduler_->commands(), view.image, TextureView::layout, buffer,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
        scheduler_->finish();  // ends with a barrier to host reads
        std::vector<std::byte> out(size);
        std::memcpy(out.data(), map, size);
        vkDestroyBuffer(d, buffer, nullptr);
        vkFreeMemory(d, memory, nullptr);
        return out;
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<TextureCache> cache_;
};

// Tiled guest bytes for `desc`, and the tight texels they hold.
struct Guest {
    std::vector<std::byte> tiled, tight;
};

Guest make_guest(const TextureDesc& desc, std::uint32_t bytes) {
    const auto layout = TextureCache::guest_layout(desc);
    EXPECT_TRUE(layout.has_value());
    Guest g;
    std::vector<std::byte> linear(layout->size);
    for (std::size_t i = 0; i < linear.size(); ++i) {
        linear[i] = static_cast<std::byte>(i * 13 + i / 97 + 1);
    }
    g.tiled.resize(layout->size);
    bb::tile::tile(*layout, linear, g.tiled);
    // The tight copy: each level's width by height texels per layer.
    for (std::uint32_t i = 0; i < layout->levels.size(); ++i) {
        const bb::tile::Level& level = layout->levels[i];
        const std::uint32_t w = std::max(desc.width >> i, 1u), h = std::max(desc.height >> i, 1u);
        const std::uint32_t layers = desc.type == image_type_2d_array ? desc.depth : 1;
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            for (std::uint32_t y = 0; y < h; ++y) {
                const std::byte* row = linear.data() + level.offset + layer * level.layer_size +
                                       std::uint64_t{y} * level.pitch * bytes;
                g.tight.insert(g.tight.end(), row, row + std::size_t{w} * bytes);
            }
        }
    }
    return g;
}

TEST_F(VkTextureCache, TiledImageArrives) {
    // Every combination is a 2D or 2D array R8G8B8A8 image in a tiled mode, with levels.
    struct Case {
        std::uint32_t mode, width, height, pitch, depth, last_level, type;
    };
    for (const Case c : {Case{14, 100, 70, 128, 1, 0, image_type_2d},
                         Case{14, 100, 70, 104, 1, 3, image_type_2d},
                         Case{9, 37, 21, 40, 3, 4, image_type_2d_array},
                         Case{8, 64, 8, 64, 1, 2, image_type_2d},
                         Case{13, 33, 33, 40, 2, 5, image_type_2d_array}}) {
        SCOPED_TRACE(testing::Message() << "mode " << c.mode << " " << c.width << "x" << c.height);
        TextureDesc desc;
        desc.address = 0x4000 + 0x100 * c.mode;
        desc.data_format = 10;
        desc.num_format = 0;
        desc.width = c.width;
        desc.height = c.height;
        desc.pitch = c.pitch;
        desc.type = c.type;
        desc.depth = c.depth;
        desc.last_level = c.last_level;
        desc.last_array = c.depth - 1;
        desc.tile_mode = c.mode;
        const Guest g = make_guest(desc, 4);
        const TextureView* view = cache_->get(desc, g.tiled);
        ASSERT_NE(view, nullptr);
        EXPECT_EQ(view->format, VK_FORMAT_R8G8B8A8_UNORM);
        EXPECT_NE(view->view, VK_NULL_HANDLE);
        const std::uint32_t levels = std::min<std::uint32_t>(
            c.last_level + 1, std::bit_width(std::max(c.width, c.height)));
        EXPECT_EQ(read_back(*view, c.width, c.height, 4,
                            c.type == image_type_2d_array ? c.depth : 1, levels),
                  g.tight);
    }
}

TEST_F(VkTextureCache, UploadsAgainOnlyWhenChanged) {
    TextureDesc desc;
    desc.address = 0x8000;
    desc.data_format = 1;  // 8, UNORM
    desc.width = desc.pitch = 64;
    desc.height = 32;
    desc.type = image_type_2d;
    desc.tile_mode = 13;
    Guest g = make_guest(desc, 1);
    const TextureView* first = cache_->get(desc, g.tiled);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(cache_->uploads(), 1u);
    // Same submission: not checked again, even with other bytes.
    std::vector<std::byte> changed = g.tiled;
    changed[5] ^= std::byte{0xff};
    EXPECT_EQ(cache_->get(desc, changed), first);
    EXPECT_EQ(cache_->uploads(), 1u);
    // A new submission with the same bytes as the upload: unchanged.
    cache_->begin_submission();
    EXPECT_EQ(cache_->get(desc, g.tiled), first);
    EXPECT_EQ(cache_->uploads(), 1u);
    // A new submission with a changed byte: uploaded again, to the same image.
    cache_->begin_submission();
    EXPECT_EQ(cache_->get(desc, changed), first);
    EXPECT_EQ(cache_->uploads(), 2u);
    EXPECT_EQ(cache_->images(), 1u);
    // Another dst_sel is another view of the same image.
    TextureDesc swapped = desc;
    swapped.dst_sel = {6, 5, 4, 7};
    const TextureView* other = cache_->get(swapped, changed);
    ASSERT_NE(other, nullptr);
    EXPECT_NE(other->view, first->view);
    EXPECT_EQ(other->image, first->image);
    EXPECT_EQ(cache_->images(), 1u);
}

TEST_F(VkTextureCache, RefusesWhatItCannotMake) {
    TextureDesc desc;
    desc.address = 0x9000;
    desc.data_format = 10;
    desc.width = desc.pitch = desc.height = 8;
    desc.type = 10;  // 3D
    std::vector<std::byte> bytes(1 << 16);
    EXPECT_EQ(cache_->get(desc, bytes), nullptr);
    desc.type = image_type_2d;
    desc.data_format = 0;  // no format
    EXPECT_EQ(cache_->get(desc, bytes), nullptr);
    desc.data_format = 10;
    desc.pitch = 4;  // under the width
    EXPECT_EQ(cache_->get(desc, bytes), nullptr);
    desc.pitch = 8;
    desc.tile_mode = 3 + 12;  // not a supported mode
    EXPECT_EQ(cache_->get(desc, bytes), nullptr);
    desc.tile_mode = 13;
    EXPECT_EQ(cache_->get(desc, std::span<const std::byte>(bytes).first(16)), nullptr);  // short
    EXPECT_NE(cache_->get(desc, bytes), nullptr);
}

TEST_F(VkTextureCache, SamplersAreCachedByState) {
    SamplerDesc s;
    s.mag_filter = s.min_filter = 1;
    s.mip_filter = 2;
    s.max_lod = 4;
    s.clamp_x = 2;
    const VkSampler a = cache_->sampler(s);
    ASSERT_NE(a, VK_NULL_HANDLE);
    EXPECT_EQ(cache_->sampler(s), a);
    EXPECT_NE(cache_->sampler(s, true), a);  // a comparing sampler
    SamplerDesc nearest = s;
    nearest.mag_filter = 0;
    EXPECT_NE(cache_->sampler(nearest), a);
    SamplerDesc unnormalized = s;
    unnormalized.force_unnormalized = true;
    EXPECT_EQ(cache_->sampler(unnormalized), VK_NULL_HANDLE);
    EXPECT_EQ(cache_->samplers(), 4u);
}

}  // namespace

TEST_F(VkTextureCache, SamplersFollowTheImagesFormat) {
    // An integer image: no linear filter, and an integer border colour. The views say so.
    TextureDesc desc;
    desc.address = 0x4000;
    desc.data_format = 4;  // 32
    desc.num_format = 4;   // UINT
    desc.width = desc.height = desc.pitch = 8;
    desc.type = image_type_2d;
    desc.tile_mode = 8;
    const Guest g = make_guest(desc, 4);
    const TextureView* view = cache_->get(desc, g.tiled);
    ASSERT_NE(view, nullptr);
    EXPECT_TRUE(view->integer);
    EXPECT_FALSE(view->filter_linear);

    SamplerDesc linear;
    linear.mag_filter = linear.min_filter = 1;
    linear.mip_filter = 2;
    linear.border_color = 2;
    const VkSampler plain = cache_->sampler(linear);
    const VkSampler limited = cache_->sampler(linear, false, {view->filter_linear, view->integer});
    EXPECT_NE(plain, VK_NULL_HANDLE);
    EXPECT_NE(limited, VK_NULL_HANDLE);
    EXPECT_NE(plain, limited);
    EXPECT_EQ(cache_->sampler(linear, false, {view->filter_linear, view->integer}), limited);
    EXPECT_EQ(cache_->samplers(), 2u);
}
