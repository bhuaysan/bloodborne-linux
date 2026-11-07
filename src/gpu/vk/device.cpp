#include "gpu/vk/device.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/log.hpp"

namespace bb::vk {

namespace {

constexpr const char* required_extensions[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME,
};

// Discrete first, then integrated, virtual, CPU and other (VkPhysicalDeviceType 0 to 4).
int type_rank(VkPhysicalDeviceType type) {
    constexpr int ranks[] = {4, 1, 0, 2, 3};
    return type >= 0 && type < 5 ? ranks[type] : 4;
}

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    const auto lower = [](char c) { return std::tolower(static_cast<unsigned char>(c)); };
    return std::search(text.begin(), text.end(), part.begin(), part.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}

// The graphics family (graphics and compute), and a family with nothing but sparse binding.
struct Families {
    std::optional<std::uint32_t> graphics, sparse;
    bool graphics_has_sparse = false;

    explicit Families(VkPhysicalDevice device) {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
        constexpr VkQueueFlags universal = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        for (std::uint32_t i = 0; i < count; ++i) {
            const VkQueueFlags flags = families[i].queueFlags;
            if (!graphics && (flags & universal) == universal) {
                graphics = i;
                graphics_has_sparse = flags & VK_QUEUE_SPARSE_BINDING_BIT;
            }
            if (!sparse && flags == VK_QUEUE_SPARSE_BINDING_BIT) {
                sparse = i;
            }
        }
    }
};

// The property and feature chains, for a Vulkan 1.3 device with the required extensions.
struct Query {
    VkPhysicalDeviceVulkan13Properties p13 = make<VkPhysicalDeviceVulkan13Properties>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES);
    VkPhysicalDeviceVulkan12Properties p12 = make<VkPhysicalDeviceVulkan12Properties>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p13);
    VkPhysicalDeviceVulkan11Properties p11 = make<VkPhysicalDeviceVulkan11Properties>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &p12);
    VkPhysicalDeviceProperties2 props = make<VkPhysicalDeviceProperties2>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11);
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR reconvergence =
        make<VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR);
    VkPhysicalDeviceVulkan13Features f13 = make<VkPhysicalDeviceVulkan13Features>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &reconvergence);
    VkPhysicalDeviceVulkan12Features f12 = make<VkPhysicalDeviceVulkan12Features>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13);
    VkPhysicalDeviceVulkan11Features f11 = make<VkPhysicalDeviceVulkan11Features>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12);
    VkPhysicalDeviceFeatures2 features = make<VkPhysicalDeviceFeatures2>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11);

    // Queries the device, or with `enable`, sets the required features for vkCreateDevice.
    explicit Query(VkPhysicalDevice device, bool enable = false) {
        if (!enable) {
            vkGetPhysicalDeviceProperties2(device, &props);
            vkGetPhysicalDeviceFeatures2(device, &features);
            return;
        }
        for (const auto& entry : wanted()) {
            *entry.first = VK_TRUE;
        }
    }
    Query(const Query&) = delete;  // the chains point into the object

    // The required features with their names.
    std::vector<std::pair<VkBool32*, const char*>> wanted() {
        return {
            {&features.features.shaderInt64, "shaderInt64"},
            {&features.features.sparseBinding, "sparseBinding"},
            {&features.features.sparseResidencyBuffer, "sparseResidencyBuffer"},
            // The tiler copies 8- and 16-bit elements through device addresses.
            {&f11.storageBuffer16BitAccess, "storageBuffer16BitAccess"},
            {&f12.storageBuffer8BitAccess, "storageBuffer8BitAccess"},
            {&f12.bufferDeviceAddress, "bufferDeviceAddress"},
            {&f12.timelineSemaphore, "timelineSemaphore"},
            // v_readfirstlane_b32 and v_readlane_b32 broadcast from a lane computed at run time.
            {&f12.subgroupBroadcastDynamicId, "subgroupBroadcastDynamicId"},
            // A translated pixel shader discards with OpDemoteToHelperInvocation.
            {&f13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation"},
            {&f13.synchronization2, "synchronization2"},
            {&f13.dynamicRendering, "dynamicRendering"},
            {&f13.subgroupSizeControl, "subgroupSizeControl"},
            {&f13.computeFullSubgroups, "computeFullSubgroups"},
            // Translated modules with subgroup operations declare MaximallyReconvergesKHR.
            {&reconvergence.shaderMaximalReconvergence, "shaderMaximalReconvergence"},
        };
    }

    std::optional<std::string> missing_feature() {
        for (auto [feature, name] : wanted()) {
            if (!*feature) {
                return name;
            }
        }
        constexpr VkSubgroupFeatureFlags ops =
            VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
            VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
        if (!(p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
            (p11.subgroupSupportedOperations & ops) != ops) {
            return "basic, vote, ballot and shuffle subgroup operations in compute";
        }
        return std::nullopt;
    }
};

// Whether `device` imports dma-bufs into the mirror's sparse buffers (VUID-VkBufferCreateInfo-
// pNext-00920 allows the handle type only when this reports it importable).
bool imports_dma_buf(VkPhysicalDevice device) {
    auto info = make<VkPhysicalDeviceExternalBufferInfo>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO);
    info.flags = mirror_buffer_flags;
    info.usage = mirror_buffer_usage;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    auto props = make<VkExternalBufferProperties>(VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES);
    vkGetPhysicalDeviceExternalBufferProperties(device, &info, &props);
    return props.externalMemoryProperties.externalMemoryFeatures &
           VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
}

// Whether `device` has the device extension `name`.
bool has_extension(VkPhysicalDevice device, const char* name) {
    std::uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
          "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()),
          "vkEnumerateDeviceExtensionProperties");
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

// Whether `device` has VK_KHR_shader_quad_control with shaderQuadControl (optional: translated
// pixel shaders declare quad control with it, docs/design/graphics.md, "Quad control").
bool has_quad_control(VkPhysicalDevice device) {
    if (!has_extension(device, VK_KHR_SHADER_QUAD_CONTROL_EXTENSION_NAME)) {
        return false;
    }
    auto quad = make<VkPhysicalDeviceShaderQuadControlFeaturesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR);
    auto features = make<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                                    &quad);
    vkGetPhysicalDeviceFeatures2(device, &features);
    return quad.shaderQuadControl;
}

// What `device` lacks of the requirements of Device::create, or nothing.
std::optional<std::string> missing(VkPhysicalDevice device, VkSurfaceKHR surface) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);  // the 1.2 structures need a 1.2 device
    if (props.apiVersion < VK_API_VERSION_1_3) {
        return "Vulkan 1.3";
    }
    std::vector<const char*> names(std::begin(required_extensions), std::end(required_extensions));
    if (surface != VK_NULL_HANDLE) {
        names.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    for (const char* name : names) {
        if (!has_extension(device, name)) {
            return name;
        }
    }
    if (auto feature = Query(device).missing_feature()) {
        return feature;
    }
    if (!imports_dma_buf(device)) {
        return "dma-buf import for sparse storage buffers with device addresses";
    }
    const Families families(device);
    if (!families.graphics) {
        return "a queue family with graphics and compute";
    }
    if (!families.sparse && !families.graphics_has_sparse) {
        return "a queue family with sparse binding";
    }
    VkBool32 presents = VK_FALSE;
    if (surface != VK_NULL_HANDLE &&
        (vkGetPhysicalDeviceSurfaceSupportKHR(device, *families.graphics, surface, &presents) !=
             VK_SUCCESS ||
         !presents)) {
        return "presentation to the window from the graphics queue family";
    }
    return std::nullopt;
}

}  // namespace

void check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(call) + " failed with VkResult " +
                                 std::to_string(static_cast<int>(result)));
    }
}

std::unique_ptr<Instance> Instance::create(std::string& why,
                                           std::span<const char* const> extensions) {
    auto app = make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "bloodborne-linux";
    app.apiVersion = VK_API_VERSION_1_3;
    auto info = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VkInstance instance = VK_NULL_HANDLE;
    if (VkResult r = vkCreateInstance(&info, nullptr, &instance); r != VK_SUCCESS) {
        why = "vkCreateInstance gave " + std::to_string(static_cast<int>(r));
        return nullptr;
    }
    why.clear();
    return std::unique_ptr<Instance>(new Instance(instance));
}

Instance::~Instance() {
    vkDestroyInstance(instance_, nullptr);
}

std::optional<std::size_t> choose_device(std::span<const Candidate> candidates,
                                         std::string_view selector, std::string& why) {
    std::optional<std::size_t> best;
    std::string rejected;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        if (!selector.empty() && !contains_ignoring_case(c.name, selector)) {
            continue;
        }
        if (c.missing) {
            rejected += (rejected.empty() ? "" : "; ") + c.name + " lacks " + *c.missing;
        } else if (!best || type_rank(c.type) < type_rank(candidates[*best].type)) {
            best = i;
        }
    }
    if (best) {
        why.clear();
    } else if (!rejected.empty()) {
        why = "no suitable Vulkan device (" + rejected + ")";
    } else if (selector.empty()) {
        why = "no Vulkan device";
    } else {
        why = "no Vulkan device matches \"" + std::string(selector) + "\"";
    }
    return best;
}

std::unique_ptr<Device> Device::create(const Instance& instance, const DeviceOptions& options,
                                       std::string& why) {
    std::uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance.handle(), &count, nullptr),
          "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> physicals(count);
    check(vkEnumeratePhysicalDevices(instance.handle(), &count, physicals.data()),
          "vkEnumeratePhysicalDevices");
    std::vector<Candidate> candidates;
    for (VkPhysicalDevice p : physicals) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(p, &props);
        candidates.push_back({props.deviceName, props.deviceType, missing(p, options.surface)});
    }
    const auto chosen = choose_device(candidates, options.device, why);
    if (!chosen) {
        return nullptr;
    }
    std::unique_ptr<Device> device(new Device);
    device->physical_ = physicals[*chosen];
    const Query query(device->physical_);
    DeviceProperties& p = device->properties_ = {query.props.properties, query.p11, query.p12,
                                                 query.p13};
    p.v11.pNext = p.v12.pNext = p.v13.pNext = nullptr;
    vkGetPhysicalDeviceMemoryProperties(device->physical_, &device->memory_);

    // Only the required features are enabled.
    Query enable(device->physical_, true);
    const Families families(device->physical_);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queues[2];
    for (int i = 0; i < 2; ++i) {
        queues[i] = make<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queues[i].queueFamilyIndex = i == 0 ? *families.graphics : families.sparse.value_or(0);
        queues[i].queueCount = 1;
        queues[i].pQueuePriorities = &priority;
    }
    // And shaderQuadControl where the device has it.
    std::vector<const char*> extensions(std::begin(required_extensions),
                                        std::end(required_extensions));
    auto quad = make<VkPhysicalDeviceShaderQuadControlFeaturesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR);
    device->quad_control_ = has_quad_control(device->physical_);
    if (device->quad_control_) {
        extensions.push_back(VK_KHR_SHADER_QUAD_CONTROL_EXTENSION_NAME);
        quad.shaderQuadControl = VK_TRUE;
        quad.pNext = std::exchange(enable.features.pNext, &quad);
    }
    if (options.surface != VK_NULL_HANDLE) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    auto info = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable.features);
    info.queueCreateInfoCount = families.sparse ? 2 : 1;
    info.pQueueCreateInfos = queues;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check(vkCreateDevice(device->physical_, &info, nullptr, &device->device_), "vkCreateDevice");

    device->graphics_.family = *families.graphics;
    vkGetDeviceQueue(device->device_, *families.graphics, 0, &device->graphics_.handle);
    if (families.sparse) {
        device->sparse_.emplace();
        device->sparse_->family = *families.sparse;
        vkGetDeviceQueue(device->device_, *families.sparse, 0, &device->sparse_->handle);
    }
    common::log(common::LogLevel::info, "gpu.vk",
                "device {} ({} {}), sparse queue family {}, quad control {}", p.core.deviceName,
                p.v12.driverName, p.v12.driverInfo, device->sparse().family,
                device->quad_control_);
    return device;
}

std::optional<std::uint32_t> Device::memory_type(std::uint32_t allowed,
                                                 VkMemoryPropertyFlags flags) const {
    for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return std::nullopt;
}

Device::~Device() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
    }
}

}  // namespace bb::vk
