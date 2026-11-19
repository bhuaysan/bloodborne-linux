// SPDX-License-Identifier: GPL-2.0-or-later
// Vulkan commands written into a byte stream on the GPU thread and replayed into a command buffer
// later (docs/design/command-recording.md, #676). DeferredCommandBuffer has the methods of the
// vk::CommandBuffer subset the renderer records; each one copies its arguments by value into the
// recorder's chunk, so no pointer into the caller's state outlives the call. Replay calls the
// same vk::CommandBuffer methods in the same order. A recorder made direct records nothing and
// calls the command buffer at once: the present and flip schedulers stay on that path. Commands
// whose replay rebuilds a Vulkan struct call the command buffer directly on that path, so the
// tests' direct trace is an independent reference for the rebuild.

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>

#include <mutex>
#include <vector>

#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// Records taken out of a recorder at a hand-over, to be replayed on the recording thread.
struct RecordedChunk {
    std::unique_ptr<std::byte[]> data;
    std::size_t size = 0;
    std::size_t capacity = 0;
};

class CommandRecorder {
public:
    // Chunk size at which the recorded commands are handed over, so the recording thread and the
    // GPU start before the frame is complete.
    static constexpr std::size_t HandOverBytes = 64 * 1024;

    using HandOverFn = Common::UniqueFunction<void, RecordedChunk&&>;

    explicit CommandRecorder(bool deferred);
    ~CommandRecorder();
    CommandRecorder(const CommandRecorder&) = delete;
    CommandRecorder& operator=(const CommandRecorder&) = delete;

    [[nodiscard]] bool Deferred() const noexcept {
        return deferred;
    }

    /// The command buffer that replay records into. Only with nothing pending.
    void SetTarget(vk::CommandBuffer cmdbuf);
    [[nodiscard]] vk::CommandBuffer Target() const noexcept {
        return target;
    }

    /// Replays every pending record into the target in recording order and empties the chunk.
    void Replay();

    /// Where a hand-over sends the chunk. Without one, a hand-over replays into the target.
    void SetHandOver(HandOverFn&& fn) {
        hand_over = std::move(fn);
    }

    /// Hands the pending records over: to the hand-over function, or replays them.
    void HandOver();

    /// Replays a chunk taken at a hand-over into cmdbuf. Any thread.
    static void ReplayChunk(vk::CommandBuffer cmdbuf, const RecordedChunk& chunk);

    /// Gives a replayed chunk's storage back for reuse. Any thread.
    void Recycle(RecordedChunk&& chunk);

    [[nodiscard]] std::size_t PendingBytes() const noexcept {
        return size;
    }
    [[nodiscard]] std::size_t PendingRecords() const noexcept {
        return records;
    }

    // An array stored behind a record, as an offset from the record's start.
    template <typename T>
    struct Arr {
        u32 offset;
        u32 count;
    };

    using ReplayFn = void (*)(vk::CommandBuffer, std::byte* record);

    // A record whose payload is a tuple of the arguments; vk::ArrayProxy arguments are copied
    // behind it and come back as proxies over the copy. F(cmdbuf, args...) records the command.
    // Direct recorders call F at once.
    template <auto F, typename... Args>
    void Emit(const Args&... args) {
        if (!deferred) {
            F(target, args...);
            return;
        }
        using Payload = std::tuple<Stored<Args>...>;
        static_assert((std::is_trivially_copyable_v<Stored<Args>> && ...));
        static_assert((!std::is_pointer_v<Stored<Args>> && ...),
                      "commands take values and array proxies, never pointers");
        static_assert(std::is_trivially_destructible_v<Payload>);
        const std::size_t rec = Begin(&Thunk<F, Payload>, sizeof(Payload), alignof(Payload));
        const Payload payload{Store(rec, args)...};
        new (data.get() + rec + HeaderBytes) Payload(payload);
        End(rec);
    }

    // The building blocks of records with a payload of their own (descriptor writes, barriers,
    // rendering info). Begin reserves the header and payload; Append copies an array behind
    // them; End closes the record and hands the chunk over once it is full. Pointers into the
    // chunk are only valid until the next Begin or Append.
    std::size_t Begin(ReplayFn fn, std::size_t payload_bytes, std::size_t payload_align);
    template <typename T>
    Arr<T> Append(std::size_t rec, const T* items, std::size_t count) {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= Align);
        if (count == 0) {
            return {0, 0};
        }
        const std::size_t at = Reserve(sizeof(T) * count);
        std::memcpy(data.get() + at, items, sizeof(T) * count);
        return {static_cast<u32>(at - rec), static_cast<u32>(count)};
    }
    // Space for count items behind the record, for the caller to fill through RecordAt.
    template <typename T>
    Arr<T> AppendSpace(std::size_t rec, std::size_t count) {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= Align);
        if (count == 0) {
            return {0, 0};
        }
        const std::size_t at = Reserve(sizeof(T) * count);
        return {static_cast<u32>(at - rec), static_cast<u32>(count)};
    }
    void End(std::size_t rec);
    std::byte* RecordAt(std::size_t rec) {
        return data.get() + rec;
    }
    // The payload's storage; construct it with placement new.
    template <typename P>
    P& PayloadAt(std::size_t rec) {
        return *reinterpret_cast<P*>(data.get() + rec + HeaderBytes);
    }

    template <typename P>
    static P& PayloadOf(std::byte* record) {
        return *std::launder(reinterpret_cast<P*>(record + HeaderBytes));
    }
    template <typename T>
    static T* Items(std::byte* record, Arr<T> arr) {
        return arr.count == 0 ? nullptr : std::launder(reinterpret_cast<T*>(record + arr.offset));
    }

private:
    static constexpr std::size_t Align = 8;
    struct Header {
        ReplayFn fn;
        u32 bytes;
        u32 reserved;
    };
    static constexpr std::size_t HeaderBytes = sizeof(Header);
    static_assert(HeaderBytes % Align == 0);

    template <typename T>
    struct StoredOf {
        using type = T;
    };
    template <typename T>
    struct StoredOf<vk::ArrayProxy<T>> {
        using type = Arr<std::remove_const_t<T>>;
    };
    template <typename T>
    using Stored = typename StoredOf<T>::type;

    template <typename T>
    const T& Store(std::size_t, const T& value) {
        return value;
    }
    template <typename T>
    Arr<std::remove_const_t<T>> Store(std::size_t rec, const vk::ArrayProxy<T>& items) {
        return Append(rec, items.data(), items.size());
    }

    template <typename T>
    static const T& Load(std::byte*, const T& value) {
        return value;
    }
    template <typename T>
    static vk::ArrayProxy<const T> Load(std::byte* record, Arr<T> arr) {
        return vk::ArrayProxy<const T>(arr.count, Items(record, arr));
    }

    template <auto F, typename Payload>
    static void Thunk(vk::CommandBuffer cmdbuf, std::byte* record) {
        std::apply([&](const auto&... args) { F(cmdbuf, Load(record, args)...); },
                   PayloadOf<Payload>(record));
    }

    // Grows the chunk by bytes, rounded up to Align, and returns the offset of the new space.
    std::size_t Reserve(std::size_t bytes);

    static void ReplayRecords(vk::CommandBuffer cmdbuf, std::byte* records, std::size_t bytes);

    // Spare chunk storage the recording thread gave back.
    static constexpr std::size_t MaxSpares = 8;

    bool deferred;
    vk::CommandBuffer target{};
    std::unique_ptr<std::byte[]> data;
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::size_t records = 0;
    HandOverFn hand_over{};
    std::mutex spares_mutex;
    std::vector<RecordedChunk> spares;
};

#define BB_VK_FORWARD(name) [](vk::CommandBuffer c, const auto&... a) { c.name(a...); }

/// The current command buffer of a scheduler: records into its CommandRecorder.
class DeferredCommandBuffer {
public:
    explicit DeferredCommandBuffer(CommandRecorder* recorder) : rec{recorder} {}

    /// The command buffer itself, for code that records through a library (ImGui). Only on a
    /// direct recorder.
    [[nodiscard]] vk::CommandBuffer Direct() const;

    void bindPipeline(vk::PipelineBindPoint bind_point, vk::Pipeline pipeline) const {
        rec->Emit<BB_VK_FORWARD(bindPipeline)>(bind_point, pipeline);
    }
    void pushDescriptorSetKHR(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
                              vk::ArrayProxy<const vk::WriteDescriptorSet> writes) const;
    void bindDescriptorSets(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                            u32 first_set, vk::ArrayProxy<const vk::DescriptorSet> sets,
                            vk::ArrayProxy<const u32> dynamic_offsets) const {
        rec->Emit<BB_VK_FORWARD(bindDescriptorSets)>(bind_point, layout, first_set, sets,
                                                     dynamic_offsets);
    }
    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset,
                       u32 size, const void* values) const {
        rec->Emit<[](vk::CommandBuffer c, vk::PipelineLayout l, vk::ShaderStageFlags s, u32 o,
                     vk::ArrayProxy<const std::byte> v) {
            c.pushConstants(l, s, o, v.size(), v.data());
        }>(layout, stages, offset,
           vk::ArrayProxy<const std::byte>(size, static_cast<const std::byte*>(values)));
    }
    void bindVertexBuffers(u32 first, u32 count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) const {
        rec->Emit<[](vk::CommandBuffer c, u32 f, vk::ArrayProxy<const vk::Buffer> b,
                     vk::ArrayProxy<const vk::DeviceSize> o) { c.bindVertexBuffers(f, b, o); }>(
            first, vk::ArrayProxy<const vk::Buffer>(count, buffers),
            vk::ArrayProxy<const vk::DeviceSize>(count, offsets));
    }
    void bindVertexBuffers2(u32 first, u32 count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) const {
        if (!rec->Deferred()) {
            rec->Target().bindVertexBuffers2(first, count, buffers, offsets, sizes, strides);
            return;
        }
        rec->Emit<[](vk::CommandBuffer c, u32 f, vk::ArrayProxy<const vk::Buffer> b,
                     vk::ArrayProxy<const vk::DeviceSize> o, vk::ArrayProxy<const vk::DeviceSize> s,
                     vk::ArrayProxy<const vk::DeviceSize> st) {
            c.bindVertexBuffers2(f, b.size(), b.data(), o.data(), s.empty() ? nullptr : s.data(),
                                 st.empty() ? nullptr : st.data());
        }>(first, vk::ArrayProxy<const vk::Buffer>(count, buffers),
           vk::ArrayProxy<const vk::DeviceSize>(count, offsets),
           vk::ArrayProxy<const vk::DeviceSize>(sizes ? count : 0, sizes),
           vk::ArrayProxy<const vk::DeviceSize>(strides ? count : 0, strides));
    }
    void bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type) const {
        rec->Emit<BB_VK_FORWARD(bindIndexBuffer)>(buffer, offset, type);
    }
    void bindIndexBuffer2KHR(vk::Buffer buffer, vk::DeviceSize offset, vk::DeviceSize size,
                             vk::IndexType type) const {
        rec->Emit<BB_VK_FORWARD(bindIndexBuffer2KHR)>(buffer, offset, size, type);
    }
    void setVertexInputEXT(
        vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> bindings,
        vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> attributes) const;

    void setViewport(u32 first, vk::ArrayProxy<const vk::Viewport> viewports) const {
        rec->Emit<BB_VK_FORWARD(setViewport)>(first, viewports);
    }
    void setViewportWithCount(vk::ArrayProxy<const vk::Viewport> viewports) const {
        rec->Emit<BB_VK_FORWARD(setViewportWithCount)>(viewports);
    }
    void setScissor(u32 first, vk::ArrayProxy<const vk::Rect2D> scissors) const {
        rec->Emit<BB_VK_FORWARD(setScissor)>(first, scissors);
    }
    void setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> scissors) const {
        rec->Emit<BB_VK_FORWARD(setScissorWithCount)>(scissors);
    }
    void setDepthTestEnable(vk::Bool32 on) const {
        rec->Emit<BB_VK_FORWARD(setDepthTestEnable)>(on);
    }
    void setDepthWriteEnable(vk::Bool32 on) const {
        rec->Emit<BB_VK_FORWARD(setDepthWriteEnable)>(on);
    }
    void setDepthCompareOp(vk::CompareOp op) const {
        rec->Emit<BB_VK_FORWARD(setDepthCompareOp)>(op);
    }
    void setDepthBoundsTestEnable(vk::Bool32 on) const {
        rec->Emit<BB_VK_FORWARD(setDepthBoundsTestEnable)>(on);
    }
    void setDepthBounds(float min, float max) const {
        rec->Emit<BB_VK_FORWARD(setDepthBounds)>(min, max);
    }
    void setDepthBiasEnable(vk::Bool32 on) const {
        rec->Emit<BB_VK_FORWARD(setDepthBiasEnable)>(on);
    }
    void setDepthBias(float constant, float clamp, float slope) const {
        rec->Emit<BB_VK_FORWARD(setDepthBias)>(constant, clamp, slope);
    }
    void setStencilTestEnable(vk::Bool32 on) const {
        rec->Emit<BB_VK_FORWARD(setStencilTestEnable)>(on);
    }
    void setStencilOp(vk::StencilFaceFlags faces, vk::StencilOp fail, vk::StencilOp pass,
                      vk::StencilOp depth_fail, vk::CompareOp compare) const {
        rec->Emit<BB_VK_FORWARD(setStencilOp)>(faces, fail, pass, depth_fail, compare);
    }
    void setStencilReference(vk::StencilFaceFlags faces, u32 value) const {
        rec->Emit<BB_VK_FORWARD(setStencilReference)>(faces, value);
    }
    void setStencilWriteMask(vk::StencilFaceFlags faces, u32 value) const {
        rec->Emit<BB_VK_FORWARD(setStencilWriteMask)>(faces, value);
    }
    void setStencilCompareMask(vk::StencilFaceFlags faces, u32 value) const {
        rec->Emit<BB_VK_FORWARD(setStencilCompareMask)>(faces, value);
    }
    void setPrimitiveRestartEnable(vk::Bool32 on) const {
        rec->Emit<BB_VK_FORWARD(setPrimitiveRestartEnable)>(on);
    }
    void setRasterizerDiscardEnable(vk::Bool32 on) const {
        rec->Emit<BB_VK_FORWARD(setRasterizerDiscardEnable)>(on);
    }
    void setCullMode(vk::CullModeFlags mode) const {
        rec->Emit<BB_VK_FORWARD(setCullMode)>(mode);
    }
    void setFrontFace(vk::FrontFace face) const {
        rec->Emit<BB_VK_FORWARD(setFrontFace)>(face);
    }
    void setBlendConstants(const float constants[4]) const {
        rec->Emit<[](vk::CommandBuffer c, const std::array<float, 4>& k) {
            c.setBlendConstants(k.data());
        }>(std::array<float, 4>{constants[0], constants[1], constants[2], constants[3]});
    }
    void setLineWidth(float width) const {
        rec->Emit<BB_VK_FORWARD(setLineWidth)>(width);
    }
    void setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags aspects) const {
        rec->Emit<BB_VK_FORWARD(setAttachmentFeedbackLoopEnableEXT)>(aspects);
    }

    void beginRendering(const vk::RenderingInfo& info) const;
    void endRendering() const {
        rec->Emit<BB_VK_FORWARD(endRendering)>();
    }
    void pipelineBarrier2(const vk::DependencyInfo& info) const;

    void draw(u32 vertices, u32 instances, u32 first_vertex, u32 first_instance) const {
        rec->Emit<BB_VK_FORWARD(draw)>(vertices, instances, first_vertex, first_instance);
    }
    void drawIndexed(u32 indices, u32 instances, u32 first_index, s32 vertex_offset,
                     u32 first_instance) const {
        rec->Emit<BB_VK_FORWARD(drawIndexed)>(indices, instances, first_index, vertex_offset,
                                              first_instance);
    }
    void drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 count, u32 stride) const {
        rec->Emit<BB_VK_FORWARD(drawIndirect)>(buffer, offset, count, stride);
    }
    void drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 count,
                             u32 stride) const {
        rec->Emit<BB_VK_FORWARD(drawIndexedIndirect)>(buffer, offset, count, stride);
    }
    void drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                           vk::DeviceSize count_offset, u32 max_count, u32 stride) const {
        rec->Emit<BB_VK_FORWARD(drawIndirectCount)>(buffer, offset, count_buffer, count_offset,
                                                    max_count, stride);
    }
    void drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset,
                                  vk::Buffer count_buffer, vk::DeviceSize count_offset,
                                  u32 max_count, u32 stride) const {
        rec->Emit<BB_VK_FORWARD(drawIndexedIndirectCount)>(buffer, offset, count_buffer,
                                                           count_offset, max_count, stride);
    }
    void dispatch(u32 x, u32 y, u32 z) const {
        rec->Emit<BB_VK_FORWARD(dispatch)>(x, y, z);
    }
    void dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const {
        rec->Emit<BB_VK_FORWARD(dispatchIndirect)>(buffer, offset);
    }

    void copyBuffer(vk::Buffer src, vk::Buffer dst,
                    vk::ArrayProxy<const vk::BufferCopy> regions) const {
        rec->Emit<BB_VK_FORWARD(copyBuffer)>(src, dst, regions);
    }
    void copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout layout,
                           vk::ArrayProxy<const vk::BufferImageCopy> regions) const {
        rec->Emit<BB_VK_FORWARD(copyBufferToImage)>(src, dst, layout, regions);
    }
    void copyImageToBuffer(vk::Image src, vk::ImageLayout layout, vk::Buffer dst,
                           vk::ArrayProxy<const vk::BufferImageCopy> regions) const {
        rec->Emit<BB_VK_FORWARD(copyImageToBuffer)>(src, layout, dst, regions);
    }
    void copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                   vk::ImageLayout dst_layout, vk::ArrayProxy<const vk::ImageCopy> regions) const {
        rec->Emit<BB_VK_FORWARD(copyImage)>(src, src_layout, dst, dst_layout, regions);
    }
    void blitImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                   vk::ImageLayout dst_layout, vk::ArrayProxy<const vk::ImageBlit> regions,
                   vk::Filter filter) const {
        rec->Emit<BB_VK_FORWARD(blitImage)>(src, src_layout, dst, dst_layout, regions, filter);
    }
    void resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                      vk::ImageLayout dst_layout,
                      vk::ArrayProxy<const vk::ImageResolve> regions) const {
        rec->Emit<BB_VK_FORWARD(resolveImage)>(src, src_layout, dst, dst_layout, regions);
    }
    void clearColorImage(vk::Image image, vk::ImageLayout layout,
                         const vk::ClearColorValue& color,
                         vk::ArrayProxy<const vk::ImageSubresourceRange> ranges) const {
        rec->Emit<BB_VK_FORWARD(clearColorImage)>(image, layout, color, ranges);
    }
    void fillBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::DeviceSize size,
                    u32 value) const {
        rec->Emit<BB_VK_FORWARD(fillBuffer)>(buffer, offset, size, value);
    }
    void updateBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::DeviceSize size,
                      const void* data) const {
        rec->Emit<[](vk::CommandBuffer c, vk::Buffer b, vk::DeviceSize o,
                     vk::ArrayProxy<const std::byte> d) {
            c.updateBuffer(b, o, d.size(), d.data());
        }>(buffer, offset,
           vk::ArrayProxy<const std::byte>(static_cast<u32>(size),
                                           static_cast<const std::byte*>(data)));
    }

private:
    CommandRecorder* rec;
};

#undef BB_VK_FORWARD

} // namespace Vulkan
