#include "launcher/gpu_probe.hpp"

#include <dlfcn.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace bb::launcher {

namespace {

// Opens the loader and lists the devices. `summary` (optional) gets one line per device.
bool probe(std::string* summary) {
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
            const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                get(instance, "vkGetPhysicalDeviceProperties2"));
            if (usable && summary != nullptr && properties2 != nullptr) {
                std::vector<VkPhysicalDevice> devices(count);
                if (enumerate(instance, &count, devices.data()) == VK_SUCCESS) {
                    for (std::uint32_t i = 0; i < count; ++i) {
                        VkPhysicalDeviceDriverProperties driver{};
                        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
                        VkPhysicalDeviceProperties2 props{};
                        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                        props.pNext = &driver;
                        properties2(devices[i], &props);
                        const auto& p = props.properties;
                        *summary += std::format(
                            "{}{} (Vulkan {}.{}.{}, driver {} {})", summary->empty() ? "" : "; ",
                            p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
                            driver.driverName, driver.driverInfo);
                    }
                }
            }
            if (destroy != nullptr) {
                destroy(instance, nullptr);
            }
        }
    }
    // The loader stays mapped: unloading drivers is not worth the risk for one probe.
    return usable;
}

}  // namespace

bool vulkan_gpu_usable() {
    return probe(nullptr);
}

std::string vulkan_gpu_summary() {
    std::string summary;
    probe(&summary);
    return summary.empty() ? "no Vulkan device found" : summary;
}

}  // namespace bb::launcher
