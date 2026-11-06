#include "gpu/runner/compute_runner.hpp"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace bb::test::vk {

namespace {

// The loader's driver filter that selects only lavapipe's manifest (lvp_icd.*.json).
constexpr const char* kLavapipeOnly = "*lvp*";

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

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceAddress address = 0;
    std::size_t size = 0;
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
    std::uint32_t memory_type = 0;  // host visible and coherent
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    Buffer draw;   // the draw block
    Buffer guest;  // guest memory
    std::uint64_t guest_base = 0;
    DeviceInfo info;

    ~State() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            destroy(guest);
            destroy(draw);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }

    // A mapped, host-coherent storage buffer with a device address.
    Buffer create_buffer(std::size_t size) {
        Buffer b;
        b.size = size;
        auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &b.buffer), "vkCreateBuffer");
        try {
            VkMemoryRequirements needs;
            vkGetBufferMemoryRequirements(device, b.buffer, &needs);
            if (!(needs.memoryTypeBits & (1u << memory_type))) {
                throw std::runtime_error("the buffer can't use host-coherent memory");
            }
            auto flags = make<VkMemoryAllocateFlagsInfo>(
                VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
            flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags);
            alloc.allocationSize = needs.size;
            alloc.memoryTypeIndex = memory_type;
            check(vkAllocateMemory(device, &alloc, nullptr, &b.memory), "vkAllocateMemory");
            check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
            check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map), "vkMapMemory");
        } catch (...) {
            destroy(b);
            throw;
        }
        std::memset(b.map, 0, size);
        auto address = make<VkBufferDeviceAddressInfo>(
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
        address.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device, &address);
        return b;
    }

    void destroy(Buffer& b) {
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);  // also unmaps it
        b = Buffer{};
    }

    // Picks lavapipe and creates the device. Returns what is missing, or nothing. With
    // `lavapipe_only`, the loader loads no driver but lavapipe, so a failed instance means that
    // lavapipe is missing.
    std::optional<std::string> init(bool lavapipe_only) {
        auto app = make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "bloodborne-linux compute runner";
        app.apiVersion = VK_API_VERSION_1_3;
        auto instance_info = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instance_info.pApplicationInfo = &app;
        if (VkResult r = vkCreateInstance(&instance_info, nullptr, &instance); r != VK_SUCCESS) {
            instance = VK_NULL_HANDLE;
            const std::string result =
                "vkCreateInstance gave " + std::to_string(static_cast<int>(r));
            if (lavapipe_only) {
                return "lavapipe driver (Mesa's Vulkan drivers; the loader, limited to "
                       "VK_LOADER_DRIVERS_SELECT=" +
                       std::string(kLavapipeOnly) + ", made no instance: " + result + ")";
            }
            return "Vulkan instance (" + result + ")";
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
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        constexpr VkMemoryPropertyFlags host =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (memory_type < memory.memoryTypeCount &&
               (memory.memoryTypes[memory_type].propertyFlags & host) != host) {
            ++memory_type;
        }
        if (queue_family == families || memory_type == memory.memoryTypeCount) {
            return "lavapipe compute queue or host-coherent memory";
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

        auto pool_info = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool_info.queueFamilyIndex = queue_family;
        check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        auto commands_info = make<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        commands_info.commandPool = pool;
        commands_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands_info.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &commands_info, &commands),
              "vkAllocateCommandBuffers");
        auto fence_info = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        check(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");

        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkDeviceAddress)};
        auto layout_info = make<VkPipelineLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout),
              "vkCreatePipelineLayout");
        draw = create_buffer(draw_block_size);
        return std::nullopt;
    }
};

// The shader module and pipeline of one run.
struct Pipeline {
    VkDevice device;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    ~Pipeline() {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
    }
};

ComputeRunner::ComputeRunner(std::unique_ptr<State> state) : state_(std::move(state)) {}

ComputeRunner::~ComputeRunner() = default;

std::unique_ptr<ComputeRunner> ComputeRunner::create(std::string& why) {
    // Only lavapipe's manifest is loaded, unless the environment selects other drivers. The
    // other drivers would be loaded and initialized for nothing.
    ::setenv("VK_LOADER_DRIVERS_SELECT", kLavapipeOnly, 0);
    const char* select = std::getenv("VK_LOADER_DRIVERS_SELECT");
    const bool lavapipe_only = select != nullptr && std::string_view(select) == kLavapipeOnly;
    auto state = std::make_unique<State>();
    if (auto missing = state->init(lavapipe_only)) {
        why = "no " + *missing;
        return nullptr;
    }
    why.clear();
    return std::unique_ptr<ComputeRunner>(new ComputeRunner(std::move(state)));
}

const DeviceInfo& ComputeRunner::info() const {
    return state_->info;
}

std::span<std::byte> ComputeRunner::guest_memory(std::uint64_t base, std::size_t size) {
    if (size == 0) {
        throw std::invalid_argument("guest memory needs at least one byte");
    }
    State& s = *state_;
    s.destroy(s.guest);
    s.guest = s.create_buffer(size);
    s.guest_base = base;
    return {static_cast<std::byte*>(s.guest.map), size};
}

void ComputeRunner::run(std::span<const std::uint32_t> spirv, const DrawBlock& draw,
                        std::array<std::uint32_t, 3> groups) {
    State& s = *state_;
    if (s.guest.buffer == VK_NULL_HANDLE) {
        throw std::logic_error("run needs guest memory first");
    }
    // Device address = guest address + bias, modulo 2^64.
    const std::uint64_t bias = s.guest.address - s.guest_base;
    auto* block = static_cast<std::byte*>(s.draw.map);
    std::memcpy(block + draw_user_data_offset, draw.user_data.data(), sizeof draw.user_data);
    std::memcpy(block + draw_address_bias_offset, &bias, sizeof bias);
    std::memcpy(block + draw_code_address_offset, &draw.code_address, sizeof draw.code_address);

    Pipeline p{s.device};
    auto module_info = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    module_info.codeSize = spirv.size_bytes();
    module_info.pCode = spirv.data();
    check(vkCreateShaderModule(s.device, &module_info, nullptr, &p.module), "vkCreateShaderModule");
    auto pipeline_info = make<VkComputePipelineCreateInfo>(
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = p.module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = s.layout;
    check(vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                   &p.pipeline),
          "vkCreateComputePipelines");

    check(vkResetCommandPool(s.device, s.pool, 0), "vkResetCommandPool");
    auto begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(s.commands, &begin), "vkBeginCommandBuffer");
    vkCmdBindPipeline(s.commands, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdPushConstants(s.commands, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof s.draw.address, &s.draw.address);
    vkCmdDispatch(s.commands, groups[0], groups[1], groups[2]);
    // The shader's writes become visible to the host once the fence signals.
    auto barrier = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(s.commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(s.commands), "vkEndCommandBuffer");

    auto submit = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s.commands;
    check(vkQueueSubmit(s.queue, 1, &submit, s.fence), "vkQueueSubmit");
    constexpr std::uint64_t timeout_ns = 30'000'000'000;
    const VkResult waited = vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, timeout_ns);
    if (waited != VK_SUCCESS) {
        vkDeviceWaitIdle(s.device);  // the pipeline must outlive the dispatch
    }
    check(vkResetFences(s.device, 1, &s.fence), "vkResetFences");
    check(waited, "vkWaitForFences");
}

}  // namespace bb::test::vk

// LeakSanitizer's suppressions for the test binaries that link the runner. Mesa allocates its
// one-time state under pthread_once and never frees it. The loader has unloaded the driver by
// the time the leak check runs, so its frames have no module name to match.
extern "C" const char* __lsan_default_suppressions() {
    return "leak:pthread_once\n";
}
