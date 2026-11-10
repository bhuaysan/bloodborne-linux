// video_core built on the host layer (src/shadps4_host): the renderer's objects come up on lavapipe
// without a window and go down again cleanly (under AddressSanitizer in the debug preset). Skips
// where there is no Vulkan loader or device.

#include <dlfcn.h>

#include <cstdlib>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <imgui.h>

#include "imgui/renderer/imgui_impl_vulkan.h"
#include "imgui/renderer/texture_manager.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {

// Selects lavapipe unless the caller chose drivers, and returns why video_core can't run, or an
// empty string. Asks the plain loader first, so that a missing loader or device skips instead of
// failing an assertion inside video_core.
std::string no_vulkan_reason() {
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    void* loader = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (loader == nullptr) {
        return "no Vulkan loader";
    }
    std::string reason;
    const auto get_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(loader, "vkGetInstanceProcAddr"));
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(
        get_proc != nullptr ? get_proc(nullptr, "vkCreateInstance") : nullptr);
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create == nullptr || create(&info, nullptr, &instance) != VK_SUCCESS) {
        reason = "no Vulkan instance";
    } else {
        const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            get_proc(instance, "vkEnumeratePhysicalDevices"));
        uint32_t count = 0;
        enumerate(instance, &count, nullptr);
        if (count == 0) {
            reason = "no Vulkan device";
        }
        reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"))(instance,
                                                                                         nullptr);
    }
    ::dlclose(loader);
    return reason;
}

// What ImGui::Core::Initialize sets up and ~Instance tears down (ImGui::Core::Shutdown), without
// the window parts: the context, the Vulkan backend and the texture worker.
void start_imgui(const Vulkan::Instance& instance) {
    ImGui::CreateContext();
    // Shutdown frees both names with delete[], as Initialize allocates them. No files here.
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    static constexpr vk::Format format = vk::Format::eB8G8R8A8Unorm;
    ImGui::Vulkan::InitInfo info{};
    info.instance = instance.GetInstance();
    info.physical_device = instance.GetPhysicalDevice();
    info.device = instance.GetDevice();
    info.queue_family = instance.GetPresentQueueFamilyIndex();
    info.queue = instance.GetPresentQueue();
    info.image_count = 2;
    info.min_allocation_size = 1024 * 1024;
    info.pipeline_rendering_create_info.colorAttachmentCount = 1;
    info.pipeline_rendering_create_info.pColorAttachmentFormats = &format;
    ASSERT_TRUE(ImGui::Vulkan::Init(info));
    ImGui::Core::TextureManager::StartWorker();
}

}  // namespace

TEST(VideoCore, CreatesAndDestroysRendererOnLavapipe) {
    if (const std::string reason = no_vulkan_reason(); !reason.empty()) {
        GTEST_SKIP() << reason;
    }
    // In the order the presenter (vk_presenter.cpp) and the GNM driver create them, and destroyed
    // in reverse. The GPU thread starts with Liverpool and stops with it.
    auto liverpool = std::make_unique<AmdGpu::Liverpool>();
    Frontend::WindowSDL window;
    {
        Vulkan::Instance instance{window, -1};
        EXPECT_TRUE(instance.GetDevice());
        if (std::string(std::getenv("VK_LOADER_DRIVERS_SELECT")) == "*lvp*") {
            EXPECT_EQ(instance.GetDriverID(), vk::DriverId::eMesaLlvmpipe);
        }
        start_imgui(instance);
        {
            Vulkan::Scheduler draw_scheduler{instance};
            Vulkan::Scheduler present_scheduler{instance};
            Vulkan::Scheduler flip_scheduler{instance};
            Vulkan::Runtime runtime{instance, draw_scheduler};
            Vulkan::Rasterizer rasterizer{instance, draw_scheduler, runtime, liverpool.get()};

            // A submission goes through the device and completes.
            const u64 tick = draw_scheduler.CurrentTick();
            draw_scheduler.Finish();
            EXPECT_GT(draw_scheduler.CurrentTick(), tick);
            EXPECT_TRUE(draw_scheduler.IsFree(tick));
        }
    }
    liverpool.reset();
}
