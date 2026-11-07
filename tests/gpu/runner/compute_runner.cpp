#include "gpu/runner/compute_runner.hpp"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/scheduler.hpp"

namespace bb::test::vk {

namespace {

using bb::vk::check;
using bb::vk::make;

// The loader's driver filter that selects only lavapipe's manifest (lvp_icd.*.json).
constexpr const char* kLavapipeOnly = "*lvp*";

DeviceInfo device_info(const bb::vk::DeviceProperties& p) {
    DeviceInfo info;
    info.name = p.core.deviceName;
    info.driver = std::string(p.v12.driverName) + " " + p.v12.driverInfo;
    info.subgroup_size = p.v11.subgroupSize;
    info.subgroup_quad = (p.v11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
                         (p.v11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0;
    info.min_subgroup_size = p.v13.minSubgroupSize;
    info.max_subgroup_size = p.v13.maxSubgroupSize;
    info.denorm_preserve_f32 = p.v12.shaderDenormPreserveFloat32;
    info.denorm_flush_f32 = p.v12.shaderDenormFlushToZeroFloat32;
    info.round_rte_f32 = p.v12.shaderRoundingModeRTEFloat32;
    info.round_rtz_f32 = p.v12.shaderRoundingModeRTZFloat32;
    info.signed_zero_inf_nan_f32 = p.v12.shaderSignedZeroInfNanPreserveFloat32;
    return info;
}

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
        << "\nf32 signed zero, inf and NaN preserved: " << signed_zero_inf_nan_f32
        << "\nquad subgroup operations: " << subgroup_quad
        << "\nshader quad control: " << quad_control << '\n';
    return out.str();
}

struct ComputeRunner::State {
    std::unique_ptr<bb::vk::Instance> instance;
    std::unique_ptr<bb::vk::Device> vk;
    std::unique_ptr<bb::vk::Scheduler> scheduler;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t memory_type = 0;  // host visible and coherent
    VkPipelineLayout layout = VK_NULL_HANDLE;
    Buffer draw;   // the draw block
    Buffer guest;  // guest memory
    std::uint64_t guest_base = 0;
    DeviceInfo info;

    ~State() {
        if (device != VK_NULL_HANDLE) {
            scheduler.reset();  // waits for the work and runs the deferred functions
            destroy(guest);
            destroy(draw);
            vkDestroyPipelineLayout(device, layout, nullptr);
        }
    }

    // A mapped, host-coherent storage buffer with a device address.
    Buffer create_buffer(std::size_t size) {
        Buffer b;
        b.size = size;
        auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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

    // Creates bbvk's device on lavapipe. Returns what is missing, or nothing. With
    // `lavapipe_only`, the loader loads no driver but lavapipe, so a failed instance means that
    // lavapipe is missing.
    std::optional<std::string> init(bool lavapipe_only) {
        std::string why;
        instance = bb::vk::Instance::create(why);
        if (!instance) {
            if (lavapipe_only) {
                return "lavapipe driver (Mesa's Vulkan drivers; the loader, limited to "
                       "VK_LOADER_DRIVERS_SELECT=" +
                       std::string(kLavapipeOnly) + ", made no instance: " + why + ")";
            }
            return "Vulkan instance (" + why + ")";
        }
        vk = bb::vk::Device::create(*instance, {.device = "llvmpipe"}, why);
        if (!vk) {
            return "lavapipe device for bbvk (Mesa's Vulkan drivers: " + why + ")";
        }
        // Lavapipe stays loaded until the process exits: Mesa keeps state that a graphics
        // pipeline's threads allocate reachable from the driver's globals only, which
        // LeakSanitizer reports as leaked once the loader has unloaded the driver.
        ::dlopen("libvulkan_lvp.so", RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
        info = device_info(vk->properties());
        info.quad_control = vk->quad_control();
        device = vk->handle();
        constexpr VkMemoryPropertyFlags host =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const auto type = vk->memory_type(~0u, host);
        if (!type) {
            return "lavapipe host-coherent memory";
        }
        memory_type = *type;
        scheduler = std::make_unique<bb::vk::Scheduler>(*vk);

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

// The shader module and pipeline of one run. The scheduler destroys them once the run's work
// has completed (release()); until then they are destroyed with the object.
struct Pipeline {
    VkDevice device;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    ~Pipeline() {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
    }

    void release(bb::vk::Scheduler& scheduler) {
        scheduler.defer([d = device, m = std::exchange(module, VK_NULL_HANDLE),
                         p = std::exchange(pipeline, VK_NULL_HANDLE)] {
            vkDestroyPipeline(d, p, nullptr);
            vkDestroyShaderModule(d, m, nullptr);
        });
    }
};

ComputeRunner::ComputeRunner(std::unique_ptr<State> state) : state_(std::move(state)) {}

ComputeRunner::ComputeRunner(ComputeRunner&& other) noexcept = default;

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
    Buffer fresh = s.create_buffer(size);
    // The old buffer goes once the work that may use it has completed.
    s.scheduler->defer([&s, old = std::exchange(s.guest, fresh)]() mutable { s.destroy(old); });
    s.guest_base = base;
    return {static_cast<std::byte*>(s.guest.map), size};
}

VkDeviceAddress ComputeRunner::draw_block(const DrawBlock& draw) {
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
    return s.draw.address;
}

void ComputeRunner::run(std::span<const std::uint32_t> spirv, const DrawBlock& draw,
                        std::array<std::uint32_t, 3> groups) {
    State& s = *state_;
    const VkDeviceAddress block = draw_block(draw);

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

    // The scheduler's submission ends with a barrier from all writes to host reads.
    const VkCommandBuffer commands = s.scheduler->commands();
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdPushConstants(commands, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof block, &block);
    vkCmdDispatch(commands, groups[0], groups[1], groups[2]);
    p.release(*s.scheduler);
    constexpr std::uint64_t timeout_ns = 30'000'000'000;
    if (!s.scheduler->wait(s.scheduler->flush(), timeout_ns)) {
        throw std::runtime_error("the dispatch did not complete within 30 s");
    }
}

bb::vk::Device& ComputeRunner::device() {
    return *state_->vk;
}

bb::vk::Scheduler& ComputeRunner::scheduler() {
    return *state_->scheduler;
}

VkBuffer ComputeRunner::guest_buffer() const {
    return state_->guest.buffer;
}

std::size_t ComputeRunner::guest_size() const {
    return state_->guest.size;
}

}  // namespace bb::test::vk

