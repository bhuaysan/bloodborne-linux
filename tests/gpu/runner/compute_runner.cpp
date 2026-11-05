#include "gpu/runner/compute_runner.hpp"

#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <vulkan/vulkan.h>

namespace bb::test::vk {

namespace {

// A Vulkan structure with its type and next pointer set and everything else zero.
template <typename T>
T make(VkStructureType type, void* next = nullptr) {
    T value{};
    value.sType = type;
    value.pNext = next;
    return value;
}

void check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(call) + " failed with VkResult " +
                                 std::to_string(static_cast<int>(result)));
    }
}

// What the device offers, read through the property and feature chains of Vulkan 1.3.
struct Query {
    VkPhysicalDeviceVulkan13Properties p13 = make<VkPhysicalDeviceVulkan13Properties>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES);
    VkPhysicalDeviceVulkan12Properties p12 = make<VkPhysicalDeviceVulkan12Properties>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p13);
    VkPhysicalDeviceVulkan11Properties p11 = make<VkPhysicalDeviceVulkan11Properties>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &p12);
    VkPhysicalDeviceProperties2 props = make<VkPhysicalDeviceProperties2>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11);
    VkPhysicalDeviceVulkan13Features f13 = make<VkPhysicalDeviceVulkan13Features>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    VkPhysicalDeviceVulkan12Features f12 = make<VkPhysicalDeviceVulkan12Features>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13);
    VkPhysicalDeviceFeatures2 features = make<VkPhysicalDeviceFeatures2>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12);

    explicit Query(VkPhysicalDevice device) {
        vkGetPhysicalDeviceProperties2(device, &props);
        vkGetPhysicalDeviceFeatures2(device, &features);
    }
    Query(const Query&) = delete;  // the chains point into the object

    // What the translator needs and the device lacks, or nothing.
    std::optional<std::string> missing() const {
        constexpr VkSubgroupFeatureFlags ops =
            VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
            VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
        if (!features.features.shaderInt64) {
            return "shaderInt64";
        }
        if (!f12.bufferDeviceAddress) {
            return "bufferDeviceAddress";
        }
        // v_readfirstlane_b32 and v_readlane_b32 broadcast from a lane computed at run time.
        if (!f12.subgroupBroadcastDynamicId) {
            return "subgroupBroadcastDynamicId";
        }
        if (!f13.subgroupSizeControl || !f13.computeFullSubgroups) {
            return "subgroup size control";
        }
        if (!(p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
            (p11.subgroupSupportedOperations & ops) != ops) {
            return "basic, vote, ballot and shuffle subgroup operations in compute";
        }
        return std::nullopt;
    }

    DeviceInfo info() const {
        DeviceInfo info;
        info.name = props.properties.deviceName;
        info.driver = std::string(p12.driverName) + " " + p12.driverInfo;
        info.subgroup_size = p11.subgroupSize;
        info.min_subgroup_size = p13.minSubgroupSize;
        info.max_subgroup_size = p13.maxSubgroupSize;
        info.denorm_preserve_f32 = p12.shaderDenormPreserveFloat32;
        info.denorm_flush_f32 = p12.shaderDenormFlushToZeroFloat32;
        info.round_rte_f32 = p12.shaderRoundingModeRTEFloat32;
        info.round_rtz_f32 = p12.shaderRoundingModeRTZFloat32;
        info.signed_zero_inf_nan_f32 = p12.shaderSignedZeroInfNanPreserveFloat32;
        return info;
    }
};

}  // namespace

std::string DeviceInfo::describe() const {
    std::ostringstream out;
    out << "device: " << name << "\ndriver: " << driver << "\nsubgroup size: " << subgroup_size
        << " (" << min_subgroup_size << " to " << max_subgroup_size << ")"
        << "\nf32 denormals preserved: " << denorm_preserve_f32
        << "\nf32 denormals flushed: " << denorm_flush_f32
        << "\nf32 round to nearest even: " << round_rte_f32
        << "\nf32 round toward zero: " << round_rtz_f32
        << "\nf32 signed zero, inf and NaN preserved: " << signed_zero_inf_nan_f32 << '\n';
    return out.str();
}

struct ComputeRunner::State {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    DeviceInfo info;

    ~State() {
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }

    // Picks lavapipe and creates the device. Returns what is missing, or nothing.
    std::optional<std::string> init() {
        auto app = make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "bloodborne-linux compute runner";
        app.apiVersion = VK_API_VERSION_1_3;
        auto instance_info = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instance_info.pApplicationInfo = &app;
        if (VkResult r = vkCreateInstance(&instance_info, nullptr, &instance); r != VK_SUCCESS) {
            instance = VK_NULL_HANDLE;
            return "Vulkan instance (vkCreateInstance gave " +
                   std::to_string(static_cast<int>(r)) + ")";
        }
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
              "vkEnumeratePhysicalDevices");
        for (VkPhysicalDevice d : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(d, &props);  // the 1.2 properties need a 1.2 device
            if (props.apiVersion >= VK_API_VERSION_1_3 &&
                Query(d).p12.driverID == VK_DRIVER_ID_MESA_LLVMPIPE) {
                physical = d;
                break;
            }
        }
        if (physical == VK_NULL_HANDLE) {
            return "lavapipe device with Vulkan 1.3 (Mesa's Vulkan drivers)";
        }
        const Query query(physical);
        if (auto what = query.missing()) {
            return "lavapipe support for " + *what;
        }
        info = query.info();

        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> family_props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, family_props.data());
        while (queue_family < families &&
               !(family_props[queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            ++queue_family;
        }
        if (queue_family == families) {
            return "lavapipe compute queue";
        }

        auto f13 = make<VkPhysicalDeviceVulkan13Features>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        f13.subgroupSizeControl = VK_TRUE;
        f13.computeFullSubgroups = VK_TRUE;
        auto f12 = make<VkPhysicalDeviceVulkan12Features>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13);
        f12.bufferDeviceAddress = VK_TRUE;
        f12.subgroupBroadcastDynamicId = VK_TRUE;
        auto features = make<VkPhysicalDeviceFeatures2>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12);
        features.features.shaderInt64 = VK_TRUE;
        const float priority = 1.0f;
        auto queue_info = make<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        auto device_info = make<VkDeviceCreateInfo>(
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features);
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        check(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        return std::nullopt;
    }
};

ComputeRunner::ComputeRunner(std::unique_ptr<State> state) : state_(std::move(state)) {}

ComputeRunner::~ComputeRunner() = default;

std::unique_ptr<ComputeRunner> ComputeRunner::create(std::string& why) {
    // Only lavapipe's manifest is loaded, unless the environment selects other drivers. The
    // other drivers would be loaded and initialized for nothing.
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);
    auto state = std::make_unique<State>();
    if (auto missing = state->init()) {
        why = "no " + *missing;
        return nullptr;
    }
    why.clear();
    return std::unique_ptr<ComputeRunner>(new ComputeRunner(std::move(state)));
}

const DeviceInfo& ComputeRunner::info() const {
    return state_->info;
}

}  // namespace bb::test::vk

// LeakSanitizer's suppressions for the test binaries that link the runner. Mesa allocates its
// one-time state under pthread_once and never frees it. The loader has unloaded the driver by
// the time the leak check runs, so its frames have no module name to match.
extern "C" const char* __lsan_default_suppressions() {
    return "leak:pthread_once\n";
}
