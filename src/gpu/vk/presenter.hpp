// bbvk's presenter: a swapchain on a surface, and one presented image per video-out flip
// (docs/design/graphics.md, "Command buffers to the first frame", item 22). Until the backend
// has render targets, a flip shows a test image: the whole surface in one colour per display
// buffer.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu/vk/device.hpp"

namespace bb::vk {

// The colour of the test image of display buffer `index`: four distinct colours, repeating.
VkClearColorValue test_color(std::uint32_t index);

class Presenter {
public:
    // Takes over `surface`, which `device` was chosen for (DeviceOptions::surface), and destroys
    // it with the presenter. `extent` is the size of the swapchain where the surface leaves the
    // size to it (Wayland, headless surfaces). The swapchain is made at the first present.
    // The instance and the device must outlive the presenter.
    Presenter(const Instance& instance, Device& device, VkSurfaceKHR surface, VkExtent2D extent);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Sets the size for surfaces that leave it to the swapchain and makes a new swapchain before
    // the next present. Any thread may call it.
    void resize(VkExtent2D extent);

    // Presents the test image of display buffer `index`. Returns false when nothing could be
    // presented: a surface of size 0 (a minimised window). Throws std::runtime_error when a
    // Vulkan call fails. One thread presents.
    bool present_test_image(std::uint32_t index);

    // The size of the current swapchain, 0x0 before the first present.
    VkExtent2D extent() const { return extent_; }
    // The number of images presented.
    std::uint64_t presented() const { return presented_; }

private:
    struct Frame {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
    };

    // Waits for the queue and replaces the swapchain. False for a surface of size 0.
    bool recreate();
    void destroy_images();

    const Instance& instance_;
    Device& device_;
    VkSurfaceKHR surface_;
    std::atomic<std::uint64_t> requested_;  // width << 32 | height
    std::atomic<bool> stale_{true};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> ready_;  // per image: the clear is done, waited on by the present
    VkCommandPool pool_ = VK_NULL_HANDLE;
    Frame frames_[2];
    std::uint64_t presented_ = 0;
};

}  // namespace bb::vk
