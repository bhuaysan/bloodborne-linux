// bbvk's presenter on lavapipe, presenting to a headless surface (VK_EXT_headless_surface), so
// that no window opens.

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/presenter.hpp"

namespace {

using namespace bb::vk;

TEST(VkPresenter, TestColorsDifferPerBuffer) {
    for (std::uint32_t a = 0; a < 4; ++a) {
        for (std::uint32_t b = a + 1; b < 4; ++b) {
            const VkClearColorValue x = test_color(a), y = test_color(b);
            EXPECT_NE(std::memcmp(x.float32, y.float32, sizeof x.float32), 0) << a << " " << b;
        }
    }
    const VkClearColorValue x = test_color(1), y = test_color(5);
    EXPECT_EQ(std::memcmp(x.float32, y.float32, sizeof x.float32), 0);
}

TEST(VkPresenter, PresentsToAHeadlessSurface) {
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    std::string why;
    const char* const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                      VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    const auto instance = Instance::create(why, extensions);
    if (!instance) {
        GTEST_SKIP() << "no Vulkan instance with a headless surface: " << why;
    }
    const auto create_surface = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        vkGetInstanceProcAddr(instance->handle(), "vkCreateHeadlessSurfaceEXT"));
    ASSERT_NE(create_surface, nullptr);
    auto info = make<VkHeadlessSurfaceCreateInfoEXT>(
        VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ASSERT_EQ(create_surface(instance->handle(), &info, nullptr, &surface), VK_SUCCESS);
    const auto device =
        Device::create(*instance, {.device = "llvmpipe", .surface = surface}, why);
    if (!device) {
        vkDestroySurfaceKHR(instance->handle(), surface, nullptr);
        GTEST_SKIP() << "no lavapipe device: " << why;
    }
    // The swapchain extension is enabled for a device with a surface.
    EXPECT_NE(vkGetDeviceProcAddr(device->handle(), "vkCreateSwapchainKHR"), nullptr);

    Presenter presenter(*instance, *device, surface, {640, 360});
    EXPECT_EQ(presenter.extent().width, 0u);  // no swapchain before the first present
    for (std::uint32_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(presenter.present_test_image(i % 2));
    }
    EXPECT_EQ(presenter.presented(), 5u);
    EXPECT_EQ(presenter.extent().width, 640u);
    EXPECT_EQ(presenter.extent().height, 360u);

    presenter.resize({320, 200});
    EXPECT_TRUE(presenter.present_test_image(2));
    EXPECT_EQ(presenter.extent().width, 320u);
    EXPECT_EQ(presenter.extent().height, 200u);

    // A minimised window: nothing is presented until it has a size again.
    presenter.resize({0, 0});
    EXPECT_FALSE(presenter.present_test_image(3));
    EXPECT_EQ(presenter.presented(), 6u);
    presenter.resize({100, 100});
    EXPECT_TRUE(presenter.present_test_image(3));
    EXPECT_EQ(presenter.presented(), 7u);
}

}  // namespace
