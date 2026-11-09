#include "gpu/vk/dispatcher.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <format>
#include <stdexcept>
#include <string>

#include "common/log.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "gpu/vk/scheduler.hpp"

namespace bb::vk {
namespace {

constexpr const char* channel = "gpu.vk";
constexpr std::uint32_t block_stride = 256;
constexpr std::uint32_t chunk_blocks = 256;  // 64 KiB per chunk
constexpr std::uint64_t group_counts_bytes = 12;

}  // namespace

Dispatcher::Dispatcher(Device& device, Scheduler& scheduler, ComputePipelineCache& pipelines,
                       const MemoryMirror& mirror)
    : device_(device), scheduler_(scheduler), pipelines_(pipelines), mirror_(mirror) {}

Dispatcher::~Dispatcher() {
    try {
        scheduler_.finish();
    } catch (const std::exception& e) {
        common::log(common::LogLevel::error, channel, "dispatcher: {}", e.what());
    }
    if (chunk_.buffer != VK_NULL_HANDLE) {
        destroy(device_.handle(), chunk_);
    }
}

void Dispatcher::destroy(VkDevice device, const Chunk& chunk) {
    vkDestroyBuffer(device, chunk.buffer, nullptr);
    vkFreeMemory(device, chunk.memory, nullptr);  // unmaps
}

void Dispatcher::make_chunk() {
    const VkDevice d = device_.handle();
    Chunk chunk;
    auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = std::uint64_t{block_stride} * chunk_blocks;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    check(vkCreateBuffer(d, &info, nullptr, &chunk.buffer), "vkCreateBuffer");
    try {
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(d, chunk.buffer, &needs);
        const auto type = device_.memory_type(needs.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!type) {
            throw std::runtime_error("no host-coherent memory for the draw blocks");
        }
        auto flags = make<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags);
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = *type;
        check(vkAllocateMemory(d, &alloc, nullptr, &chunk.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(d, chunk.buffer, chunk.memory, 0), "vkBindBufferMemory");
        void* map = nullptr;
        check(vkMapMemory(d, chunk.memory, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory");
        chunk.map = static_cast<std::byte*>(map);
        auto address = make<VkBufferDeviceAddressInfo>(
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
        address.buffer = chunk.buffer;
        chunk.address = vkGetBufferDeviceAddress(d, &address);
    } catch (...) {
        destroy(d, chunk);
        throw;
    }
    if (chunk_.buffer != VK_NULL_HANDLE) {
        // The recorded dispatches still read the full chunk; free it once they completed.
        scheduler_.defer([d, old = chunk_] { destroy(d, old); });
    }
    chunk_ = chunk;
}

VkDeviceAddress Dispatcher::new_block(std::byte*& block) {
    if (chunk_.buffer == VK_NULL_HANDLE || chunk_.used == chunk_blocks) {
        make_chunk();
    }
    const std::uint64_t offset = std::uint64_t{chunk_.used++} * block_stride;
    block = chunk_.map + offset;
    std::memset(block, 0, gcn::draw_block_size);
    return chunk_.address + offset;
}

void Dispatcher::sync(pm4::SyncPoint point) {
    try {
        if (point == pm4::SyncPoint::before_wait) {
            scheduler_.flush();
        } else if (scheduler_.recording() || scheduler_.completed() < scheduler_.submitted()) {
            scheduler_.finish();
        }
    } catch (const std::exception& e) {
        common::log(common::LogLevel::error, channel, "dispatch sync: {}", e.what());
    }
}

void Dispatcher::dispatch(const pm4::RegisterMirror& regs, const pm4::DispatchCall& call) {
    try {
        record(regs, call);
    } catch (const std::exception& e) {
        ++failed_;
        common::log(common::LogLevel::error, channel, "dispatch failed: {}", e.what());
    }
}

void Dispatcher::record(const pm4::RegisterMirror& regs, const pm4::DispatchCall& call) {
    std::string why;
    const auto state = compute_state(regs, why);
    const ComputePipeline* pipeline = state ? pipelines_.get(*state, why) : nullptr;
    if (pipeline == nullptr) {
        ++untranslated_;
        const std::uint64_t program = state ? state->code_address : 0;
        if (std::ranges::find(logged_, program) == logged_.end()) {
            logged_.push_back(program);
            common::log(common::LogLevel::warn, channel, "dispatch skipped: {}", why);
        }
        return;
    }

    // The indirect counts live in the mirror: buffer k at the window offsets of its 2 GiB.
    VkBuffer counts = VK_NULL_HANDLE;
    std::uint64_t counts_offset = 0;
    if (call.indirect != 0) {
        const std::uint64_t window = call.indirect - mirror_.window_start();
        const std::uint64_t k = window / mirror_buffer_size;
        counts_offset = window % mirror_buffer_size;
        if (call.indirect < mirror_.window_start() || k >= mirror_.size() / mirror_buffer_size ||
            counts_offset + group_counts_bytes > mirror_buffer_size) {
            throw std::runtime_error(
                std::format("indirect counts at {:#x} are outside the mirror", call.indirect));
        }
        counts = mirror_.buffer(k);
    }

    std::byte* block = nullptr;
    const VkDeviceAddress address = new_block(block);
    const auto user = regs.user_data(pm4::Stage::cs);
    std::memcpy(block, user.data(), user.size_bytes());
    const std::uint64_t table = mirror_.bias_table();
    std::memcpy(block + gcn::draw_address_bias_offset, &table, sizeof table);
    std::memcpy(block + gcn::draw_code_address_offset, &state->code_address, 8);

    const VkCommandBuffer commands = scheduler_.commands();
    // A dispatch may read what the one before wrote: order them.
    auto barrier = make<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    auto dependency = make<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
    vkCmdPushConstants(commands, pipeline->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       compute_push_size, &address);
    if (counts != VK_NULL_HANDLE) {
        vkCmdDispatchIndirect(commands, counts, counts_offset);
    } else {
        vkCmdDispatch(commands, call.groups[0], call.groups[1], call.groups[2]);
    }
    ++recorded_;
}

}  // namespace bb::vk
