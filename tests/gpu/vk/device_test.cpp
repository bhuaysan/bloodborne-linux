// bbvk's device choice, and the device core on lavapipe.

#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"

namespace {

using namespace bb::vk;

std::optional<std::size_t> choose(const std::vector<Candidate>& candidates,
                                  std::string_view selector, std::string& why) {
    return choose_device(candidates, selector, why);
}

const std::vector<Candidate> devices = {
    {"llvmpipe (LLVM 22.1.0, 256 bits)", VK_PHYSICAL_DEVICE_TYPE_CPU, {}},
    {"AMD Radeon (RADV RAPHAEL)", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, {}},
    {"Old GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, "Vulkan 1.3"},
    {"AMD Radeon RX 7800 XT (RADV NAVI32)", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, {}},
    {"Second CPU", VK_PHYSICAL_DEVICE_TYPE_CPU, {}},
};

TEST(VkChooseDevice, AutomaticPrefersSuitableDiscrete) {
    std::string why = "stale";
    EXPECT_EQ(choose(devices, "", why), 3u);
    EXPECT_EQ(why, "");
    // Without the discrete GPU: integrated before CPU; within a type, enumeration order.
    EXPECT_EQ(choose({devices[0], devices[1]}, "", why), 1u);
    EXPECT_EQ(choose({devices[0], devices[4]}, "", why), 0u);
}

TEST(VkChooseDevice, SelectorMatchesName) {
    std::string why;
    EXPECT_EQ(choose(devices, "LLVMpipe", why), 0u);
    EXPECT_EQ(choose(devices, "radv", why), 3u);  // both match, the discrete one wins
    EXPECT_EQ(choose(devices, "cpu", why), 4u);
}

TEST(VkChooseDevice, ReasonsWhenNothingFits) {
    std::string why;
    EXPECT_EQ(choose({}, "", why), std::nullopt);
    EXPECT_EQ(why, "no Vulkan device");
    EXPECT_EQ(choose(devices, "nvidia", why), std::nullopt);
    EXPECT_EQ(why, "no Vulkan device matches \"nvidia\"");
    EXPECT_EQ(choose(devices, "old", why), std::nullopt);
    EXPECT_EQ(why, "no suitable Vulkan device (Old GPU lacks Vulkan 1.3)");
    EXPECT_EQ(choose({devices[2]}, "", why), std::nullopt);
    EXPECT_EQ(why, "no suitable Vulkan device (Old GPU lacks Vulkan 1.3)");
}

// The device core on lavapipe, which has every required feature.
TEST(VkDevice, CreatesLavapipe) {
    // Only lavapipe's driver is loaded, unless the environment selects other drivers.
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    std::string why;
    const auto instance = Instance::create(why);
    if (!instance) {
        GTEST_SKIP() << "no Vulkan instance: " << why;
    }
    const auto device = Device::create(*instance, {.device = "llvmpipe"}, why);
    if (!device) {
        GTEST_SKIP() << "no lavapipe device: " << why;
    }
    const DeviceProperties& p = device->properties();
    EXPECT_NE(std::string(p.core.deviceName).find("llvmpipe"), std::string::npos);
    EXPECT_GE(p.core.apiVersion, VK_API_VERSION_1_3);
    EXPECT_EQ(p.v12.driverID, VK_DRIVER_ID_MESA_LLVMPIPE);
    EXPECT_NE(device->handle(), VK_NULL_HANDLE);
    EXPECT_NE(device->graphics().handle, VK_NULL_HANDLE);
    // The mirror's sparse buffers can take dma-bufs (device choice requires it).
    auto info = make<VkPhysicalDeviceExternalBufferInfo>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO);
    info.flags = mirror_buffer_flags;
    info.usage = mirror_buffer_usage;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    auto external = make<VkExternalBufferProperties>(VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES);
    vkGetPhysicalDeviceExternalBufferProperties(device->physical(), &info, &external);
    EXPECT_TRUE(external.externalMemoryProperties.externalMemoryFeatures &
                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
    // The dma-buf extensions are enabled: their entry point resolves on the device.
    EXPECT_NE(vkGetDeviceProcAddr(device->handle(), "vkGetMemoryFdPropertiesKHR"), nullptr);
    // Lavapipe has one queue family with everything, so the sparse queue is the graphics queue.
    EXPECT_EQ(&device->sparse(), &device->graphics());
}

}  // namespace

// Mesa's one-time state, as in the compute runner (tests/gpu/runner/compute_runner.cpp).
extern "C" const char* __lsan_default_suppressions() {
    return "leak:pthread_once\n";
}
