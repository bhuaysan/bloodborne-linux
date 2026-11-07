#include "gpu/vk/presenter.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace bb::vk {
namespace {

std::uint64_t pack(VkExtent2D e) {
    return std::uint64_t{e.width} << 32 | e.height;
}

// A barrier of the whole colour image from `from` to `to`.
void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    auto barrier = make<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

bool is_unorm(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM;
}

}  // namespace

VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    // The display buffers are 8-bit sRGB-encoded colour; a UNORM format keeps the values.
    const auto srgb = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& f) {
        return is_unorm(f.format) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (srgb != formats.end()) {
        return *srgb;
    }
    const auto unorm =
        std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& f) { return is_unorm(f.format); });
    return unorm != formats.end() ? *unorm : formats.at(0);
}

VkClearColorValue test_color(std::uint32_t index) {
    static constexpr VkClearColorValue colors[] = {
        {{0.6f, 0.1f, 0.1f, 1.0f}},  // red
        {{0.1f, 0.5f, 0.2f, 1.0f}},  // green
        {{0.1f, 0.2f, 0.6f, 1.0f}},  // blue
        {{0.6f, 0.5f, 0.1f, 1.0f}},  // yellow
    };
    return colors[index % std::size(colors)];
}

Presenter::Presenter(const Instance& instance, Device& device, VkSurfaceKHR surface,
                     VkExtent2D extent)
    : instance_(instance), device_(device), surface_(surface), requested_(pack(extent)) {
    const VkDevice d = device_.handle();
    auto pool_info = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_.graphics().family;
    check(vkCreateCommandPool(d, &pool_info, nullptr, &pool_), "vkCreateCommandPool");
    for (Frame& f : frames_) {
        auto alloc =
            make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(d, &alloc, &f.commands), "vkAllocateCommandBuffers");
        auto fence = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(d, &fence, nullptr, &f.done), "vkCreateFence");
        auto semaphore = make<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        check(vkCreateSemaphore(d, &semaphore, nullptr, &f.acquired), "vkCreateSemaphore");
    }
}

Presenter::~Presenter() {
    const VkDevice d = device_.handle();
    {
        const std::lock_guard lock(device_.graphics().lock);
        vkQueueWaitIdle(device_.graphics().handle);
    }
    destroy_images();
    vkDestroySwapchainKHR(d, swapchain_, nullptr);
    for (Frame& f : frames_) {
        vkDestroyFence(d, f.done, nullptr);
        vkDestroySemaphore(d, f.acquired, nullptr);
    }
    vkDestroyCommandPool(d, pool_, nullptr);
    vkDestroySurfaceKHR(instance_.handle(), surface_, nullptr);
}

void Presenter::resize(VkExtent2D extent) {
    requested_ = pack(extent);
    stale_ = true;
}

void Presenter::destroy_images() {
    for (VkSemaphore s : ready_) {
        vkDestroySemaphore(device_.handle(), s, nullptr);
    }
    ready_.clear();
    images_.clear();
}

bool Presenter::recreate() {
    const VkDevice d = device_.handle();
    const VkPhysicalDevice p = device_.physical();
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, surface_, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
        const std::uint64_t r = requested_;
        if (static_cast<std::uint32_t>(r >> 32) == 0 || static_cast<std::uint32_t>(r) == 0) {
            return false;
        }
        extent.width = std::clamp(static_cast<std::uint32_t>(r >> 32), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<std::uint32_t>(r), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        throw std::runtime_error("the surface's images can't be transfer destinations");
    }
    if (format_.format == VK_FORMAT_UNDEFINED) {
        std::uint32_t count = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(p, surface_, &count, nullptr),
              "vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(p, surface_, &count, formats.data()),
              "vkGetPhysicalDeviceSurfaceFormatsKHR");
        if (formats.empty()) {
            throw std::runtime_error("the surface has no formats");
        }
        format_ = choose_surface_format(formats);
    }
    {
        const std::lock_guard lock(device_.graphics().lock);
        check(vkQueueWaitIdle(device_.graphics().handle), "vkQueueWaitIdle");
    }
    auto info = make<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
    info.surface = surface_;
    info.minImageCount = caps.maxImageCount == 0
                             ? caps.minImageCount + 1
                             : std::min(caps.minImageCount + 1, caps.maxImageCount);
    info.imageFormat = format_.format;
    info.imageColorSpace = format_.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & info.compositeAlpha)) {
        info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
            caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);  // the lowest one
    }
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported; flips come at 60 Hz
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(d, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
    destroy_images();
    vkDestroySwapchainKHR(d, swapchain_, nullptr);
    swapchain_ = swapchain;
    extent_ = extent;
    std::uint32_t count = 0;
    check(vkGetSwapchainImagesKHR(d, swapchain_, &count, nullptr), "vkGetSwapchainImagesKHR");
    images_.resize(count);
    check(vkGetSwapchainImagesKHR(d, swapchain_, &count, images_.data()),
          "vkGetSwapchainImagesKHR");
    for (std::uint32_t i = 0; i < count; ++i) {
        auto semaphore = make<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        check(vkCreateSemaphore(d, &semaphore, nullptr, &ready_.emplace_back()),
              "vkCreateSemaphore");
    }
    return true;
}

bool Presenter::present_test_image(std::uint32_t index) {
    const VkDevice d = device_.handle();
    Frame& frame = frames_[presented_ % std::size(frames_)];
    check(vkWaitForFences(d, 1, &frame.done, VK_TRUE, std::numeric_limits<std::uint64_t>::max()),
          "vkWaitForFences");
    std::uint32_t image = 0;
    for (int attempt = 0;; ++attempt) {
        if (stale_.exchange(false) && !recreate()) {
            stale_ = true;  // try again at the next present
            return false;
        }
        const VkResult r = vkAcquireNextImageKHR(d, swapchain_,
                                                 std::numeric_limits<std::uint64_t>::max(),
                                                 frame.acquired, VK_NULL_HANDLE, &image);
        if (r == VK_SUBOPTIMAL_KHR) {
            stale_ = true;  // use the image, replace the swapchain next time
        } else if (r == VK_ERROR_OUT_OF_DATE_KHR && attempt == 0) {
            stale_ = true;
            continue;
        } else {
            check(r, "vkAcquireNextImageKHR");
        }
        break;
    }
    check(vkResetFences(d, 1, &frame.done), "vkResetFences");

    auto begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(frame.commands, &begin), "vkBeginCommandBuffer");
    const VkImage target = images_[image];
    transition(frame.commands, target, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue color = test_color(index);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(frame.commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                         &range);
    transition(frame.commands, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    check(vkEndCommandBuffer(frame.commands), "vkEndCommandBuffer");

    auto wait = make<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    wait.semaphore = frame.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    auto signal = make<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    signal.semaphore = ready_[image];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    auto buffer = make<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    buffer.commandBuffer = frame.commands;
    auto submit = make<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &buffer;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    auto present = make<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &ready_[image];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image;
    VkResult presented;
    {
        Queue& queue = device_.graphics();
        const std::lock_guard lock(queue.lock);
        check(vkQueueSubmit2(queue.handle, 1, &submit, frame.done), "vkQueueSubmit2");
        presented = vkQueuePresentKHR(queue.handle, &present);
    }
    if (presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR) {
        stale_ = true;
    } else {
        check(presented, "vkQueuePresentKHR");
    }
    ++presented_;
    return true;
}

}  // namespace bb::vk
