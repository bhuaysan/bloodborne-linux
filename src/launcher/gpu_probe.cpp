#include "launcher/gpu_probe.hpp"

#include <dlfcn.h>

#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace bb::launcher {

bool vulkan_gpu_usable() {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        return false;
    }
    bool usable = false;
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (get != nullptr) {
        const auto create =
            reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        VkInstance instance = VK_NULL_HANDLE;
        if (create != nullptr && create(&info, nullptr, &instance) == VK_SUCCESS) {
            const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
                get(instance, "vkEnumeratePhysicalDevices"));
            const auto destroy =
                reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
            std::uint32_t count = 0;
            usable = enumerate != nullptr && enumerate(instance, &count, nullptr) == VK_SUCCESS &&
                     count > 0;
            if (destroy != nullptr) {
                destroy(instance, nullptr);
            }
        }
    }
    // The loader stays mapped: unloading drivers is not worth the risk for one probe.
    return usable;
}

}  // namespace bb::launcher
