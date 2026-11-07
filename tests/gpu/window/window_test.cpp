// The game window on SDL's offscreen video driver, which needs no display and makes its Vulkan
// surfaces with VK_EXT_headless_surface, presenting through bbvk on lavapipe.

#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/presenter.hpp"
#include "gpu/window/window.hpp"

namespace {

TEST(Window, PresentsOnTheOffscreenDriver) {
    ::setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    std::string why;
    const auto window = bb::window::Window::create("window test", 320, 200, why);
    if (!window) {
        GTEST_SKIP() << "no offscreen window: " << why;
    }
    const VkExtent2D size = window->pixel_size();
    EXPECT_EQ(size.width, 320u);
    EXPECT_EQ(size.height, 200u);
    const auto extensions = window->instance_extensions();
    ASSERT_FALSE(extensions.empty());
    const auto instance = bb::vk::Instance::create(why, extensions);
    if (!instance) {
        GTEST_SKIP() << "no Vulkan instance with the window's extensions: " << why;
    }
    const VkSurfaceKHR surface = window->create_surface(instance->handle());
    const auto device =
        bb::vk::Device::create(*instance, {.device = "llvmpipe", .surface = surface}, why);
    if (!device) {
        vkDestroySurfaceKHR(instance->handle(), surface, nullptr);
        GTEST_SKIP() << "no lavapipe device: " << why;
    }
    bb::vk::Presenter presenter(*instance, *device, surface, size);
    EXPECT_TRUE(presenter.present_test_image(0));
    EXPECT_TRUE(presenter.present_test_image(1));
    EXPECT_EQ(presenter.extent().width, 320u);
    EXPECT_TRUE(window->pump(0, [&](VkExtent2D e) { presenter.resize(e); }));
}

// SDL's own SIGINT and SIGTERM handlers would turn Ctrl-C into a window close.
TEST(Window, LeavesTheSignalsAlone) {
    ::setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
    std::string why;
    const auto window = bb::window::Window::create("window test", 64, 64, why);
    if (!window) {
        GTEST_SKIP() << "no offscreen window: " << why;
    }
    for (const int signal : {SIGINT, SIGTERM}) {
        struct sigaction action {};
        ASSERT_EQ(::sigaction(signal, nullptr, &action), 0);
        EXPECT_EQ(action.sa_handler, SIG_DFL) << "signal " << signal;
    }
}

// bb-run falls back to headless on this exception (open_display).
TEST(Window, CreateSurfaceThrowsWithoutTheWindowsExtensions) {
    ::setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    std::string why;
    const auto window = bb::window::Window::create("window test", 64, 64, why);
    if (!window) {
        GTEST_SKIP() << "no offscreen window: " << why;
    }
    const auto instance = bb::vk::Instance::create(why, {});
    if (!instance) {
        GTEST_SKIP() << "no Vulkan instance: " << why;
    }
    EXPECT_THROW(window->create_surface(instance->handle()), std::runtime_error);
}

}  // namespace

// Mesa's one-time state, as in the compute runner (tests/gpu/runner/compute_runner.cpp).
extern "C" const char* __lsan_default_suppressions() {
    return "leak:pthread_once\n";
}
