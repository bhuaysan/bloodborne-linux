// The target image keys, and the image cache on lavapipe.

#include <cstdlib>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/image_cache.hpp"

namespace {

using namespace bb::vk;

TEST(ImageKeys, FromTargets) {
    bb::pm4::ColorTarget c;
    c.address = 0x10000;
    c.extent = {1920, 1080};
    c.slices = 2;
    c.format = 10;
    c.number_type = 6;
    c.comp_swap = 1;
    c.tile_mode = 13;
    c.samples = 4;
    EXPECT_EQ(color_image_key(c),
              (ImageKey{0x10000, VK_FORMAT_B8G8R8A8_SRGB, {1920, 1080}, 2, 4, 13}));
    c.format = 7;  // 11_11_10 has no Vulkan format with its layout
    EXPECT_EQ(color_image_key(c), std::nullopt);

    bb::pm4::DepthTarget d;
    d.z_format = 3;
    d.stencil = true;
    d.z_address = 0x20000;
    d.stencil_address = 0x30000;
    d.extent = {640, 360};
    d.tile_mode = 2;
    EXPECT_EQ(depth_image_key(d),
              (ImageKey{0x20000, VK_FORMAT_D32_SFLOAT_S8_UINT, {640, 360}, 1, 1, 2}));
    d.z_format = 0;  // stencil alone
    EXPECT_EQ(depth_image_key(d)->address, 0x30000u);
    d.z_format = 2;
    EXPECT_EQ(depth_image_key(d), std::nullopt);
}

class VkImageCache : public testing::Test {
protected:
    void SetUp() override {
        ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);  // as in device_test.cpp
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = "llvmpipe"}, why);
        if (!device_) {
            GTEST_SKIP() << "no lavapipe device: " << why;
        }
        cache_ = std::make_unique<ImageCache>(*device_);
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<ImageCache> cache_;
};

TEST_F(VkImageCache, OneImagePerAddressAndDescriptor) {
    const ImageKey key{0x10000, VK_FORMAT_R8G8B8A8_UNORM, {1920, 1080}, 2, 1, 13};
    Image* image = cache_->get(key);
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(cache_->get(key), image);
    EXPECT_EQ(image->key, key);
    EXPECT_EQ(image->format, key.format);
    EXPECT_EQ(image->aspects, VK_IMAGE_ASPECT_COLOR_BIT);
    EXPECT_NE(image->handle, VK_NULL_HANDLE);
    EXPECT_EQ(image->layout, VK_IMAGE_LAYOUT_UNDEFINED);
    // Any difference in address or descriptor is another image.
    ImageKey others[] = {key, key, key, key, key, key};
    others[0].address += 0x100;
    others[1].format = VK_FORMAT_R16G16_SFLOAT;
    others[2].extent.height = 1088;
    others[3].slices = 1;
    others[4].samples = 4;
    others[5].tile_mode = 8;
    for (const ImageKey& other : others) {
        Image* got = cache_->get(other);
        ASSERT_NE(got, nullptr);
        EXPECT_NE(got, image);
        EXPECT_EQ(cache_->get(other), got);
    }
}

TEST_F(VkImageCache, DepthFormatsAndSupport) {
    Image* d = cache_->get({0x20000, VK_FORMAT_D32_SFLOAT_S8_UINT, {640, 360}, 1, 1, 2});
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->aspects, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    // D16_UNORM_S8_UINT is optional: kept where the device has it, otherwise D32_SFLOAT_S8_UINT.
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(device_->physical(), VK_FORMAT_D16_UNORM_S8_UINT, &p);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const bool d16s8 = (p.optimalTilingFeatures & needed) == needed;
    Image* s = cache_->get({0x30000, VK_FORMAT_D16_UNORM_S8_UINT, {640, 360}, 1, 1, 2});
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->format, d16s8 ? VK_FORMAT_D16_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT);
    // A sample count the device lacks, and an empty extent, give no image, also when asked again.
    const auto counts = device_->properties().core.limits.framebufferColorSampleCounts;
    const ImageKey two{0x40000, VK_FORMAT_R8G8B8A8_UNORM, {64, 64}, 1, 2, 0};
    EXPECT_EQ(cache_->get(two) != nullptr, (counts & VK_SAMPLE_COUNT_2_BIT) != 0);
    EXPECT_EQ(cache_->get(two) != nullptr, (counts & VK_SAMPLE_COUNT_2_BIT) != 0);
    EXPECT_EQ(cache_->get({0x40000, VK_FORMAT_R8G8B8A8_UNORM, {0, 64}, 1, 1, 0}), nullptr);
}

}  // namespace
