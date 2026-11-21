#include "launcher/gpu_probe.hpp"

#include <dlfcn.h>

#include <cstdint>
#include <functional>

#include <vulkan/vulkan_core.h>

namespace bb::launcher {

namespace {

// Calls `use` with the loader's instance and its vkGetInstanceProcAddr. False when there is no
// loader or no instance.
bool with_instance(const std::function<void(VkInstance, PFN_vkGetInstanceProcAddr)>& use) {
    // The loader stays mapped: unloading drivers is not worth the risk for one probe.
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        return false;
    }
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (get == nullptr) {
        return false;
    }
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create == nullptr || create(&info, nullptr, &instance) != VK_SUCCESS) {
        return false;
    }
    use(instance, get);
    if (const auto destroy =
            reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"))) {
        destroy(instance, nullptr);
    }
    return true;
}

std::vector<VkPhysicalDevice> devices(VkInstance instance, PFN_vkGetInstanceProcAddr get) {
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get(instance, "vkEnumeratePhysicalDevices"));
    std::uint32_t count = 0;
    if (enumerate == nullptr || enumerate(instance, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkPhysicalDevice> out(count);
    if (enumerate(instance, &count, out.data()) < 0) {  // VK_INCOMPLETE still lists them
        return {};
    }
    out.resize(count);
    return out;
}

}  // namespace

bool vulkan_gpu_usable() {
    bool usable = false;
    with_instance([&](VkInstance instance, PFN_vkGetInstanceProcAddr get) {
        usable = !devices(instance, get).empty();
    });
    return usable;
}

std::vector<std::string> vulkan_gpu_names() {
    std::vector<std::string> names;
    with_instance([&](VkInstance instance, PFN_vkGetInstanceProcAddr get) {
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            get(instance, "vkGetPhysicalDeviceProperties"));
        if (properties == nullptr) {
            return;
        }
        for (VkPhysicalDevice device : devices(instance, get)) {
            VkPhysicalDeviceProperties p{};
            properties(device, &p);
            if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
                names.emplace_back(p.deviceName);
            }
        }
    });
    return names;
}

}  // namespace bb::launcher
