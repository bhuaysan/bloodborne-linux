// SPDX-License-Identifier: GPL-2.0-or-later
// The recorder's chunk, its replay and the commands whose arguments hold pointers
// (docs/design/command-recording.md, #676).

#include "video_core/renderer_vulkan/vk_command_recorder.h"

#include <algorithm>

#include "common/assert.h"
#include "common/frame_profile.hpp"

namespace Vulkan {

namespace {

constexpr std::size_t AlignUp(std::size_t value, std::size_t align) {
    return (value + align - 1) & ~(align - 1);
}

} // namespace

CommandRecorder::CommandRecorder(bool deferred_) : deferred{deferred_} {}

CommandRecorder::~CommandRecorder() = default;

void CommandRecorder::SetTarget(vk::CommandBuffer cmdbuf) {
    ASSERT_MSG(size == 0, "command buffer changed with {} records pending", records);
    target = cmdbuf;
}

std::size_t CommandRecorder::Reserve(std::size_t bytes) {
    const std::size_t aligned = AlignUp(bytes, Align);
    if (size + aligned > capacity) {
        const std::size_t grown = std::max({capacity * 2, size + aligned, 2 * HandOverBytes});
        auto bigger = std::make_unique_for_overwrite<std::byte[]>(grown);
        if (size != 0) {
            std::memcpy(bigger.get(), data.get(), size);
        }
        data = std::move(bigger);
        capacity = grown;
    }
    const std::size_t at = size;
    size += aligned;
    return at;
}

std::size_t CommandRecorder::Begin(ReplayFn fn, std::size_t payload_bytes,
                                   std::size_t payload_align) {
    ASSERT(payload_align <= Align);
    const std::size_t rec = Reserve(HeaderBytes + payload_bytes);
    const Header header{.fn = fn, .bytes = 0, .reserved = 0};
    std::memcpy(data.get() + rec, &header, sizeof(header));
    return rec;
}

void CommandRecorder::End(std::size_t rec) {
    const u32 bytes = static_cast<u32>(size - rec);
    std::memcpy(data.get() + rec + offsetof(Header, bytes), &bytes, sizeof(bytes));
    ++records;
    if (size >= HandOverBytes) {
        Replay();
    }
}

void CommandRecorder::Replay() {
    if (size == 0) {
        return;
    }
    bb::common::frame_profile::Scope profile(bb::common::frame_profile::Part::replay);
    std::byte* record = data.get();
    std::byte* const end = record + size;
    while (record < end) {
        Header header;
        std::memcpy(&header, record, sizeof(header));
        header.fn(target, record);
        record += header.bytes;
    }
    size = 0;
    records = 0;
}

vk::CommandBuffer DeferredCommandBuffer::Direct() const {
    ASSERT_MSG(!rec->Deferred(), "a deferred command buffer has no direct handle");
    return rec->Target();
}

namespace {

// Which pointer of a descriptor write its copied infos go to.
enum class InfoKind : u32 { None, Image, Buffer, TexelView };

struct InfoRef {
    u32 offset;
    InfoKind kind;
};

struct PushDescriptors {
    vk::PipelineLayout layout;
    vk::PipelineBindPoint bind_point;
    u32 set;
    CommandRecorder::Arr<vk::WriteDescriptorSet> writes;
    CommandRecorder::Arr<InfoRef> infos;
};

void ReplayPushDescriptors(vk::CommandBuffer cmdbuf, std::byte* record) {
    const auto& p = CommandRecorder::PayloadOf<PushDescriptors>(record);
    vk::WriteDescriptorSet* const writes = CommandRecorder::Items(record, p.writes);
    const InfoRef* const infos = CommandRecorder::Items(record, p.infos);
    // The chunk is replayed once, so the copied writes are pointed at the copied infos in place.
    for (u32 i = 0; i < p.writes.count; ++i) {
        std::byte* const at = record + infos[i].offset;
        switch (infos[i].kind) {
        case InfoKind::Image:
            writes[i].pImageInfo = std::launder(reinterpret_cast<vk::DescriptorImageInfo*>(at));
            break;
        case InfoKind::Buffer:
            writes[i].pBufferInfo = std::launder(reinterpret_cast<vk::DescriptorBufferInfo*>(at));
            break;
        case InfoKind::TexelView:
            writes[i].pTexelBufferView = std::launder(reinterpret_cast<vk::BufferView*>(at));
            break;
        case InfoKind::None:
            break;
        }
    }
    cmdbuf.pushDescriptorSetKHR(
        p.bind_point, p.layout, p.set,
        vk::ArrayProxy<const vk::WriteDescriptorSet>(p.writes.count, writes));
}

} // namespace

void DeferredCommandBuffer::pushDescriptorSetKHR(
    vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
    vk::ArrayProxy<const vk::WriteDescriptorSet> writes) const {
    if (!rec->Deferred()) {
        rec->Target().pushDescriptorSetKHR(bind_point, layout, set, writes);
        return;
    }
    const std::size_t at = rec->Begin(&ReplayPushDescriptors, sizeof(PushDescriptors),
                                      alignof(PushDescriptors));
    const u32 count = writes.size();
    const auto stored_writes = rec->Append(at, writes.data(), count);
    // One ref per write, filled while its infos are copied behind the record.
    const auto stored_infos = rec->AppendSpace<InfoRef>(at, count);
    for (u32 i = 0; i < count; ++i) {
        const vk::WriteDescriptorSet& write = writes.data()[i];
        ASSERT_MSG(write.pNext == nullptr, "descriptor write with a pNext chain");
        InfoRef ref{0, InfoKind::None};
        if (write.pImageInfo != nullptr) {
            ref = {rec->Append(at, write.pImageInfo, write.descriptorCount).offset,
                   InfoKind::Image};
        } else if (write.pBufferInfo != nullptr) {
            ref = {rec->Append(at, write.pBufferInfo, write.descriptorCount).offset,
                   InfoKind::Buffer};
        } else if (write.pTexelBufferView != nullptr) {
            ref = {rec->Append(at, write.pTexelBufferView, write.descriptorCount).offset,
                   InfoKind::TexelView};
        }
        std::memcpy(rec->RecordAt(at) + stored_infos.offset + sizeof(InfoRef) * i, &ref,
                    sizeof(ref));
    }
    new (&rec->PayloadAt<PushDescriptors>(at)) PushDescriptors{
        .layout = layout,
        .bind_point = bind_point,
        .set = set,
        .writes = stored_writes,
        .infos = stored_infos,
    };
    rec->End(at);
}

void DeferredCommandBuffer::setVertexInputEXT(
    vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> bindings,
    vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> attributes) const {
    DEBUG_ASSERT(std::ranges::all_of(bindings, [](const auto& b) { return !b.pNext; }) &&
                 std::ranges::all_of(attributes, [](const auto& a) { return !a.pNext; }));
    rec->Emit<[](vk::CommandBuffer c, vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> b,
                 vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> a) {
        c.setVertexInputEXT(b, a);
    }>(bindings, attributes);
}

void DeferredCommandBuffer::beginRendering(const vk::RenderingInfo& info) const {
    ASSERT_MSG(info.pNext == nullptr, "rendering info with a pNext chain");
    // Depth and stencil go by value; an absent one is marked by an empty view and a zero mask.
    std::array<vk::RenderingAttachmentInfo, 2> depth_stencil{};
    u32 present = 0;
    if (info.pDepthAttachment != nullptr) {
        std::memcpy(&depth_stencil[0], info.pDepthAttachment, sizeof(depth_stencil[0]));
        present |= 1;
    }
    if (info.pStencilAttachment != nullptr) {
        std::memcpy(&depth_stencil[1], info.pStencilAttachment, sizeof(depth_stencil[1]));
        present |= 2;
    }
    rec->Emit<[](vk::CommandBuffer c, vk::RenderingFlags flags, const vk::Rect2D& area, u32 layers,
                 u32 view_mask, vk::ArrayProxy<const vk::RenderingAttachmentInfo> colors,
                 const std::array<vk::RenderingAttachmentInfo, 2>& ds, u32 ds_present) {
        c.beginRendering(vk::RenderingInfo{
            .flags = flags,
            .renderArea = area,
            .layerCount = layers,
            .viewMask = view_mask,
            .colorAttachmentCount = colors.size(),
            .pColorAttachments = colors.data(),
            .pDepthAttachment = (ds_present & 1) ? &ds[0] : nullptr,
            .pStencilAttachment = (ds_present & 2) ? &ds[1] : nullptr,
        });
    }>(info.flags, info.renderArea, info.layerCount, info.viewMask,
       vk::ArrayProxy<const vk::RenderingAttachmentInfo>(info.colorAttachmentCount,
                                                         info.pColorAttachments),
       depth_stencil, present);
}

void DeferredCommandBuffer::pipelineBarrier2(const vk::DependencyInfo& info) const {
    ASSERT_MSG(info.pNext == nullptr, "dependency info with a pNext chain");
    rec->Emit<[](vk::CommandBuffer c, vk::DependencyFlags flags,
                 vk::ArrayProxy<const vk::MemoryBarrier2> memory,
                 vk::ArrayProxy<const vk::BufferMemoryBarrier2> buffers,
                 vk::ArrayProxy<const vk::ImageMemoryBarrier2> images) {
        c.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = flags,
            .memoryBarrierCount = memory.size(),
            .pMemoryBarriers = memory.data(),
            .bufferMemoryBarrierCount = buffers.size(),
            .pBufferMemoryBarriers = buffers.data(),
            .imageMemoryBarrierCount = images.size(),
            .pImageMemoryBarriers = images.data(),
        });
    }>(info.dependencyFlags,
       vk::ArrayProxy<const vk::MemoryBarrier2>(info.memoryBarrierCount, info.pMemoryBarriers),
       vk::ArrayProxy<const vk::BufferMemoryBarrier2>(info.bufferMemoryBarrierCount,
                                                      info.pBufferMemoryBarriers),
       vk::ArrayProxy<const vk::ImageMemoryBarrier2>(info.imageMemoryBarrierCount,
                                                     info.pImageMemoryBarriers));
}

} // namespace Vulkan
