// The deferred command buffer (#676, docs/design/command-recording.md) replays every command with
// the arguments a direct call passes. Fakes in vulkan.hpp's dynamic dispatcher log each vkCmd*
// call with the contents behind its pointers; a script of every command runs once on a direct
// recorder and once on a deferred one whose sources are overwritten right after each call. The
// script crosses the hand-over size, so replay also happens in the middle of recording. Needs no
// Vulkan device.

#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_command_recorder.h"

namespace {

std::string* trace = nullptr;

template <typename T>
void Put(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    trace->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void PutN(const T* items, uint32_t count) {
    Put(count);
    if (count != 0) {
        trace->append(reinterpret_cast<const char*>(items), sizeof(T) * count);
    }
}

void Name(const char* name) {
    trace->append(name);
    trace->push_back('\0');
}

// A descriptor write without its pointers, then what they point to.
void PutWrite(const VkWriteDescriptorSet& write) {
    VkWriteDescriptorSet copy = write;
    copy.pImageInfo = nullptr;
    copy.pBufferInfo = nullptr;
    copy.pTexelBufferView = nullptr;
    Put(copy);
    PutN(write.pImageInfo, write.pImageInfo ? write.descriptorCount : 0);
    PutN(write.pBufferInfo, write.pBufferInfo ? write.descriptorCount : 0);
    PutN(write.pTexelBufferView, write.pTexelBufferView ? write.descriptorCount : 0);
}

void InstallFakes(vk::detail::DispatchLoaderDynamic& d) {
    d.vkCmdBindPipeline = [](VkCommandBuffer, VkPipelineBindPoint bp, VkPipeline p) {
        Name("bindPipeline"), Put(bp), Put(p);
    };
    d.vkCmdPushDescriptorSetKHR = [](VkCommandBuffer, VkPipelineBindPoint bp, VkPipelineLayout l,
                                     uint32_t set, uint32_t n, const VkWriteDescriptorSet* w) {
        Name("pushDescriptorSet"), Put(bp), Put(l), Put(set), Put(n);
        for (uint32_t i = 0; i < n; ++i) {
            PutWrite(w[i]);
        }
    };
    d.vkCmdBindDescriptorSets = [](VkCommandBuffer, VkPipelineBindPoint bp, VkPipelineLayout l,
                                   uint32_t first, uint32_t n, const VkDescriptorSet* sets,
                                   uint32_t nd, const uint32_t* dyn) {
        Name("bindDescriptorSets"), Put(bp), Put(l), Put(first), PutN(sets, n), PutN(dyn, nd);
    };
    d.vkCmdPushConstants = [](VkCommandBuffer, VkPipelineLayout l, VkShaderStageFlags s,
                              uint32_t o, uint32_t size, const void* v) {
        Name("pushConstants"), Put(l), Put(s), Put(o), PutN(static_cast<const char*>(v), size);
    };
    d.vkCmdBindVertexBuffers = [](VkCommandBuffer, uint32_t first, uint32_t n, const VkBuffer* b,
                                  const VkDeviceSize* o) {
        Name("bindVertexBuffers"), Put(first), PutN(b, n), PutN(o, n);
    };
    d.vkCmdBindVertexBuffers2 = [](VkCommandBuffer, uint32_t first, uint32_t n, const VkBuffer* b,
                                   const VkDeviceSize* o, const VkDeviceSize* s,
                                   const VkDeviceSize* st) {
        Name("bindVertexBuffers2"), Put(first), PutN(b, n), PutN(o, n), PutN(s, s ? n : 0),
            PutN(st, st ? n : 0);
    };
    d.vkCmdBindIndexBuffer = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkIndexType t) {
        Name("bindIndexBuffer"), Put(b), Put(o), Put(t);
    };
    d.vkCmdBindIndexBuffer2KHR = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkDeviceSize s,
                                    VkIndexType t) {
        Name("bindIndexBuffer2"), Put(b), Put(o), Put(s), Put(t);
    };
    d.vkCmdSetVertexInputEXT = [](VkCommandBuffer, uint32_t nb,
                                  const VkVertexInputBindingDescription2EXT* b, uint32_t na,
                                  const VkVertexInputAttributeDescription2EXT* a) {
        Name("setVertexInput"), PutN(b, nb), PutN(a, na);
    };
    d.vkCmdSetViewport = [](VkCommandBuffer, uint32_t first, uint32_t n, const VkViewport* v) {
        Name("setViewport"), Put(first), PutN(v, n);
    };
    d.vkCmdSetViewportWithCount = [](VkCommandBuffer, uint32_t n, const VkViewport* v) {
        Name("setViewportWithCount"), PutN(v, n);
    };
    d.vkCmdSetScissor = [](VkCommandBuffer, uint32_t first, uint32_t n, const VkRect2D* s) {
        Name("setScissor"), Put(first), PutN(s, n);
    };
    d.vkCmdSetScissorWithCount = [](VkCommandBuffer, uint32_t n, const VkRect2D* s) {
        Name("setScissorWithCount"), PutN(s, n);
    };
    d.vkCmdSetDepthTestEnable = [](VkCommandBuffer, VkBool32 v) { Name("depthTest"), Put(v); };
    d.vkCmdSetDepthWriteEnable = [](VkCommandBuffer, VkBool32 v) { Name("depthWrite"), Put(v); };
    d.vkCmdSetDepthCompareOp = [](VkCommandBuffer, VkCompareOp v) { Name("depthOp"), Put(v); };
    d.vkCmdSetDepthBoundsTestEnable = [](VkCommandBuffer, VkBool32 v) {
        Name("boundsTest"), Put(v);
    };
    d.vkCmdSetDepthBounds = [](VkCommandBuffer, float a, float b) {
        Name("bounds"), Put(a), Put(b);
    };
    d.vkCmdSetDepthBiasEnable = [](VkCommandBuffer, VkBool32 v) { Name("biasOn"), Put(v); };
    d.vkCmdSetDepthBias = [](VkCommandBuffer, float a, float b, float c) {
        Name("bias"), Put(a), Put(b), Put(c);
    };
    d.vkCmdSetStencilTestEnable = [](VkCommandBuffer, VkBool32 v) { Name("stencil"), Put(v); };
    d.vkCmdSetStencilOp = [](VkCommandBuffer, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b,
                             VkStencilOp c, VkCompareOp op) {
        Name("stencilOp"), Put(f), Put(a), Put(b), Put(c), Put(op);
    };
    d.vkCmdSetStencilReference = [](VkCommandBuffer, VkStencilFaceFlags f, uint32_t v) {
        Name("stencilRef"), Put(f), Put(v);
    };
    d.vkCmdSetStencilWriteMask = [](VkCommandBuffer, VkStencilFaceFlags f, uint32_t v) {
        Name("stencilWrite"), Put(f), Put(v);
    };
    d.vkCmdSetStencilCompareMask = [](VkCommandBuffer, VkStencilFaceFlags f, uint32_t v) {
        Name("stencilCompare"), Put(f), Put(v);
    };
    d.vkCmdSetPrimitiveRestartEnable = [](VkCommandBuffer, VkBool32 v) {
        Name("restart"), Put(v);
    };
    d.vkCmdSetRasterizerDiscardEnable = [](VkCommandBuffer, VkBool32 v) {
        Name("discard"), Put(v);
    };
    d.vkCmdSetCullMode = [](VkCommandBuffer, VkCullModeFlags v) { Name("cull"), Put(v); };
    d.vkCmdSetFrontFace = [](VkCommandBuffer, VkFrontFace v) { Name("front"), Put(v); };
    d.vkCmdSetBlendConstants = [](VkCommandBuffer, const float v[4]) {
        Name("blend"), PutN(v, 4);
    };
    d.vkCmdSetLineWidth = [](VkCommandBuffer, float v) { Name("line"), Put(v); };
    d.vkCmdSetAttachmentFeedbackLoopEnableEXT = [](VkCommandBuffer, VkImageAspectFlags v) {
        Name("feedback"), Put(v);
    };
    d.vkCmdBeginRendering = [](VkCommandBuffer, const VkRenderingInfo* r) {
        Name("beginRendering"), Put(r->pNext), Put(r->flags), Put(r->renderArea),
            Put(r->layerCount), Put(r->viewMask), PutN(r->pColorAttachments,
                                                       r->colorAttachmentCount);
        PutN(r->pDepthAttachment, r->pDepthAttachment ? 1 : 0);
        PutN(r->pStencilAttachment, r->pStencilAttachment ? 1 : 0);
    };
    d.vkCmdEndRendering = [](VkCommandBuffer) { Name("endRendering"); };
    d.vkCmdPipelineBarrier2 = [](VkCommandBuffer, const VkDependencyInfo* i) {
        Name("barrier"), Put(i->pNext), Put(i->dependencyFlags),
            PutN(i->pMemoryBarriers, i->memoryBarrierCount),
            PutN(i->pBufferMemoryBarriers, i->bufferMemoryBarrierCount),
            PutN(i->pImageMemoryBarriers, i->imageMemoryBarrierCount);
    };
    d.vkCmdDraw = [](VkCommandBuffer, uint32_t a, uint32_t b, uint32_t c, uint32_t e) {
        Name("draw"), Put(a), Put(b), Put(c), Put(e);
    };
    d.vkCmdDrawIndexed = [](VkCommandBuffer, uint32_t a, uint32_t b, uint32_t c, int32_t e,
                            uint32_t f) {
        Name("drawIndexed"), Put(a), Put(b), Put(c), Put(e), Put(f);
    };
    d.vkCmdDrawIndirect = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
        Name("drawIndirect"), Put(b), Put(o), Put(n), Put(s);
    };
    d.vkCmdDrawIndexedIndirect = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o, uint32_t n,
                                    uint32_t s) {
        Name("drawIndexedIndirect"), Put(b), Put(o), Put(n), Put(s);
    };
    d.vkCmdDrawIndirectCount = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkBuffer cb,
                                  VkDeviceSize co, uint32_t n, uint32_t s) {
        Name("drawIndirectCount"), Put(b), Put(o), Put(cb), Put(co), Put(n), Put(s);
    };
    d.vkCmdDrawIndexedIndirectCount = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkBuffer cb,
                                         VkDeviceSize co, uint32_t n, uint32_t s) {
        Name("drawIndexedIndirectCount"), Put(b), Put(o), Put(cb), Put(co), Put(n), Put(s);
    };
    d.vkCmdDispatch = [](VkCommandBuffer, uint32_t x, uint32_t y, uint32_t z) {
        Name("dispatch"), Put(x), Put(y), Put(z);
    };
    d.vkCmdDispatchIndirect = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o) {
        Name("dispatchIndirect"), Put(b), Put(o);
    };
    d.vkCmdCopyBuffer = [](VkCommandBuffer, VkBuffer s, VkBuffer t, uint32_t n,
                           const VkBufferCopy* r) {
        Name("copyBuffer"), Put(s), Put(t), PutN(r, n);
    };
    d.vkCmdCopyBufferToImage = [](VkCommandBuffer, VkBuffer s, VkImage t, VkImageLayout l,
                                  uint32_t n, const VkBufferImageCopy* r) {
        Name("copyBufferToImage"), Put(s), Put(t), Put(l), PutN(r, n);
    };
    d.vkCmdCopyImageToBuffer = [](VkCommandBuffer, VkImage s, VkImageLayout l, VkBuffer t,
                                  uint32_t n, const VkBufferImageCopy* r) {
        Name("copyImageToBuffer"), Put(s), Put(l), Put(t), PutN(r, n);
    };
    d.vkCmdCopyImage = [](VkCommandBuffer, VkImage s, VkImageLayout sl, VkImage t,
                          VkImageLayout tl, uint32_t n, const VkImageCopy* r) {
        Name("copyImage"), Put(s), Put(sl), Put(t), Put(tl), PutN(r, n);
    };
    d.vkCmdBlitImage = [](VkCommandBuffer, VkImage s, VkImageLayout sl, VkImage t,
                          VkImageLayout tl, uint32_t n, const VkImageBlit* r, VkFilter f) {
        Name("blitImage"), Put(s), Put(sl), Put(t), Put(tl), PutN(r, n), Put(f);
    };
    d.vkCmdResolveImage = [](VkCommandBuffer, VkImage s, VkImageLayout sl, VkImage t,
                             VkImageLayout tl, uint32_t n, const VkImageResolve* r) {
        Name("resolveImage"), Put(s), Put(sl), Put(t), Put(tl), PutN(r, n);
    };
    d.vkCmdClearColorImage = [](VkCommandBuffer, VkImage i, VkImageLayout l,
                                const VkClearColorValue* c, uint32_t n,
                                const VkImageSubresourceRange* r) {
        Name("clearColorImage"), Put(i), Put(l), Put(*c), PutN(r, n);
    };
    d.vkCmdFillBuffer = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkDeviceSize s,
                           uint32_t v) { Name("fillBuffer"), Put(b), Put(o), Put(s), Put(v); };
    d.vkCmdUpdateBuffer = [](VkCommandBuffer, VkBuffer b, VkDeviceSize o, VkDeviceSize s,
                             const void* v) {
        Name("updateBuffer"), Put(b), Put(o), PutN(static_cast<const char*>(v), uint32_t(s));
    };
}

template <typename H>
H Handle(uint64_t value) {
    return H{reinterpret_cast<typename H::CType>(value)};
}

// A value of T filled with a pattern that depends on seed; padding included, so the bytes a
// direct call sees are exactly the bytes of the copy.
template <typename T>
T Pattern(uint32_t seed) {
    std::array<unsigned char, sizeof(T)> bytes;
    for (size_t i = 0; i < sizeof(T); ++i) {
        bytes[i] = static_cast<unsigned char>(seed * 31 + i * 7);
    }
    return std::bit_cast<T>(bytes);
}

// Overwrites a source right after the call, so a deferred record that kept a pointer to it
// replays garbage.
template <typename T>
void Clobber(T& value) {
    std::memset(static_cast<void*>(&value), 0xAB, sizeof(T));
}
template <typename T>
void Clobber(std::vector<T>& values) {
    for (auto& v : values) {
        Clobber(v);
    }
}

template <typename T>
std::vector<T> Patterns(uint32_t seed, uint32_t count) {
    std::vector<T> out;
    for (uint32_t i = 0; i < count; ++i) {
        out.push_back(Pattern<T>(seed + i));
    }
    return out;
}

template <typename T>
T NoNext(T value) {
    value.pNext = nullptr;
    return value;
}

// Every command of DeferredCommandBuffer, round times, with fresh sources per call.
void Script(const Vulkan::DeferredCommandBuffer& cmd, uint32_t round) {
    const uint32_t s = round * 1000;
    const auto buf = Handle<vk::Buffer>(0x1000 + round);
    const auto img = Handle<vk::Image>(0x2000 + round);
    const auto layout = Handle<vk::PipelineLayout>(0x3000 + round);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, Handle<vk::Pipeline>(0x4000 + round));
    {
        auto images = Patterns<vk::DescriptorImageInfo>(s + 1, 3);
        auto buffers = Patterns<vk::DescriptorBufferInfo>(s + 2, 2);
        auto views = Patterns<vk::BufferView>(s + 3, 4);
        std::vector<vk::WriteDescriptorSet> writes(4);
        for (uint32_t i = 0; i < 4; ++i) {
            writes[i] = NoNext(Pattern<vk::WriteDescriptorSet>(s + 10 + i));
            writes[i].pImageInfo = nullptr;
            writes[i].pBufferInfo = nullptr;
            writes[i].pTexelBufferView = nullptr;
        }
        writes[0].descriptorCount = 3, writes[0].pImageInfo = images.data();
        writes[1].descriptorCount = 2, writes[1].pBufferInfo = buffers.data();
        writes[2].descriptorCount = 4, writes[2].pTexelBufferView = views.data();
        writes[3].descriptorCount = 1, writes[3].pBufferInfo = buffers.data() + 1;
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        Clobber(images), Clobber(buffers), Clobber(views), Clobber(writes);
    }
    {
        auto sets = Patterns<vk::DescriptorSet>(s + 20, 2);
        std::vector<uint32_t> dyn{s + 1, s + 2, s + 3};
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, sets, dyn);
        Clobber(sets), Clobber(dyn);
    }
    {
        auto data = Pattern<std::array<char, 100>>(s + 30);
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eFragment, 16, 100, data.data());
        Clobber(data);
    }
    {
        auto bufs = Patterns<vk::Buffer>(s + 40, 5);
        auto offs = Patterns<vk::DeviceSize>(s + 41, 5);
        auto sizes = Patterns<vk::DeviceSize>(s + 42, 5);
        auto strides = Patterns<vk::DeviceSize>(s + 43, 5);
        cmd.bindVertexBuffers(2, 5, bufs.data(), offs.data());
        cmd.bindVertexBuffers2(0, 5, bufs.data(), offs.data(), sizes.data(), strides.data());
        cmd.bindVertexBuffers2(0, 5, bufs.data(), offs.data(), nullptr, strides.data());
        Clobber(bufs), Clobber(offs), Clobber(sizes), Clobber(strides);
    }
    cmd.bindIndexBuffer(buf, 64, vk::IndexType::eUint16);
    cmd.bindIndexBuffer2KHR(buf, 128, 4096, vk::IndexType::eUint32);
    {
        std::vector<vk::VertexInputBindingDescription2EXT> b(3);
        std::vector<vk::VertexInputAttributeDescription2EXT> a(5);
        for (uint32_t i = 0; i < 3; ++i) {
            b[i] = NoNext(Pattern<vk::VertexInputBindingDescription2EXT>(s + 50 + i));
        }
        for (uint32_t i = 0; i < 5; ++i) {
            a[i] = NoNext(Pattern<vk::VertexInputAttributeDescription2EXT>(s + 60 + i));
        }
        cmd.setVertexInputEXT(b, a);
        Clobber(b), Clobber(a);
    }
    {
        auto vp = Patterns<vk::Viewport>(s + 70, 3);
        auto sc = Patterns<vk::Rect2D>(s + 71, 2);
        cmd.setViewport(1, vp);
        cmd.setViewportWithCount(vp);
        cmd.setScissor(0, sc);
        cmd.setScissorWithCount(sc);
        Clobber(vp), Clobber(sc);
    }
    cmd.setDepthTestEnable(round & 1);
    cmd.setDepthWriteEnable(true);
    cmd.setDepthCompareOp(vk::CompareOp::eGreaterOrEqual);
    cmd.setDepthBoundsTestEnable(false);
    cmd.setDepthBounds(0.25f + round, 0.75f);
    cmd.setDepthBiasEnable(true);
    cmd.setDepthBias(1.5f, 2.5f, 3.5f + round);
    cmd.setStencilTestEnable(true);
    cmd.setStencilOp(vk::StencilFaceFlagBits::eFront, vk::StencilOp::eKeep,
                     vk::StencilOp::eReplace, vk::StencilOp::eIncrementAndClamp,
                     vk::CompareOp::eLess);
    cmd.setStencilReference(vk::StencilFaceFlagBits::eBack, s + 5);
    cmd.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, 0xF0);
    cmd.setStencilCompareMask(vk::StencilFaceFlagBits::eFront, 0x0F);
    cmd.setPrimitiveRestartEnable(true);
    cmd.setRasterizerDiscardEnable(false);
    cmd.setCullMode(vk::CullModeFlagBits::eBack);
    cmd.setFrontFace(vk::FrontFace::eClockwise);
    {
        float k[4] = {0.1f, 0.2f, 0.3f, float(round)};
        cmd.setBlendConstants(k);
        Clobber(k);
    }
    cmd.setLineWidth(2.0f);
    cmd.setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlagBits::eColor);
    {
        std::vector<vk::RenderingAttachmentInfo> colors(round % 4 + 1);
        for (uint32_t i = 0; i < colors.size(); ++i) {
            colors[i] = NoNext(Pattern<vk::RenderingAttachmentInfo>(s + 80 + i));
        }
        auto depth = NoNext(Pattern<vk::RenderingAttachmentInfo>(s + 90));
        auto stencil = NoNext(Pattern<vk::RenderingAttachmentInfo>(s + 91));
        vk::RenderingInfo info{
            .renderArea = Pattern<vk::Rect2D>(s + 92),
            .layerCount = 2,
            .colorAttachmentCount = static_cast<uint32_t>(colors.size()),
            .pColorAttachments = colors.data(),
            .pDepthAttachment = round % 2 ? &depth : nullptr,
            .pStencilAttachment = round % 3 ? &stencil : nullptr,
        };
        cmd.beginRendering(info);
        Clobber(colors), Clobber(depth), Clobber(stencil), Clobber(info);
    }
    cmd.draw(3, 1, s, 0);
    cmd.drawIndexed(36, 2, 6, -int32_t(round), 1);
    cmd.drawIndirect(buf, 16, 4, 20);
    cmd.drawIndexedIndirect(buf, 32, 5, 24);
    cmd.drawIndirectCount(buf, 0, Handle<vk::Buffer>(0x5000), 8, 9, 16);
    cmd.drawIndexedIndirectCount(buf, 64, Handle<vk::Buffer>(0x5001), 12, 7, 20);
    cmd.endRendering();
    {
        std::vector<vk::MemoryBarrier2> m(1);
        std::vector<vk::BufferMemoryBarrier2> b(2);
        std::vector<vk::ImageMemoryBarrier2> i(round % 3);
        for (auto& x : m) x = NoNext(Pattern<vk::MemoryBarrier2>(s + 100));
        for (auto& x : b) x = NoNext(Pattern<vk::BufferMemoryBarrier2>(s + 101));
        for (auto& x : i) x = NoNext(Pattern<vk::ImageMemoryBarrier2>(s + 102));
        vk::DependencyInfo info{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = m.data(),
            .bufferMemoryBarrierCount = 2,
            .pBufferMemoryBarriers = b.data(),
            .imageMemoryBarrierCount = static_cast<uint32_t>(i.size()),
            .pImageMemoryBarriers = i.data(),
        };
        cmd.pipelineBarrier2(info);
        Clobber(m), Clobber(b), Clobber(i), Clobber(info);
    }
    cmd.dispatch(round, 2, 3);
    cmd.dispatchIndirect(buf, 48);
    {
        auto bc = Patterns<vk::BufferCopy>(s + 110, 3);
        auto bic = Patterns<vk::BufferImageCopy>(s + 111, 2);
        auto ic = Patterns<vk::ImageCopy>(s + 112, 2);
        auto bl = Patterns<vk::ImageBlit>(s + 113, 1);
        auto rs = Patterns<vk::ImageResolve>(s + 114, 1);
        auto rg = Patterns<vk::ImageSubresourceRange>(s + 115, 2);
        auto color = Pattern<vk::ClearColorValue>(s + 116);
        cmd.copyBuffer(buf, Handle<vk::Buffer>(0x6000), bc);
        cmd.copyBufferToImage(buf, img, vk::ImageLayout::eTransferDstOptimal, bic);
        cmd.copyImageToBuffer(img, vk::ImageLayout::eTransferSrcOptimal, buf, bic);
        cmd.copyImage(img, vk::ImageLayout::eGeneral, Handle<vk::Image>(0x7000),
                      vk::ImageLayout::eTransferDstOptimal, ic);
        cmd.blitImage(img, vk::ImageLayout::eGeneral, Handle<vk::Image>(0x7001),
                      vk::ImageLayout::eTransferDstOptimal, bl, vk::Filter::eLinear);
        cmd.resolveImage(img, vk::ImageLayout::eGeneral, Handle<vk::Image>(0x7002),
                         vk::ImageLayout::eTransferDstOptimal, rs);
        cmd.clearColorImage(img, vk::ImageLayout::eTransferDstOptimal, color, rg);
        Clobber(bc), Clobber(bic), Clobber(ic), Clobber(bl), Clobber(rs), Clobber(rg),
            Clobber(color);
    }
    cmd.fillBuffer(buf, 0, 256, s + 7);
    {
        auto data = Pattern<std::array<char, 52>>(s + 120);
        cmd.updateBuffer(buf, 4, 52, data.data());
        Clobber(data);
    }
}

std::string Record(bool deferred, uint32_t rounds, size_t* replays_during) {
    std::string out;
    trace = &out;
    Vulkan::CommandRecorder recorder{deferred};
    recorder.SetTarget(Handle<vk::CommandBuffer>(0xC0FFEE));
    const Vulkan::DeferredCommandBuffer cmd{&recorder};
    for (uint32_t round = 0; round < rounds; ++round) {
        Script(cmd, round);
        EXPECT_LT(recorder.PendingBytes(), Vulkan::CommandRecorder::HandOverBytes);
    }
    if (replays_during != nullptr) {
        *replays_during = out.size();
    }
    EXPECT_EQ(recorder.PendingRecords() != 0, deferred);
    recorder.Replay();
    EXPECT_EQ(recorder.PendingBytes(), 0u);
    EXPECT_EQ(recorder.PendingRecords(), 0u);
    trace = nullptr;
    return out;
}

TEST(CommandRecorder, ReplaysEveryCommandAsADirectCallWould) {
    vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
    const vk::detail::DispatchLoaderDynamic saved = dispatcher;
    InstallFakes(dispatcher);

    // One round, well under the hand-over size: nothing reaches the fakes before Replay.
    size_t early = 0;
    const std::string direct_one = Record(false, 1, nullptr);
    EXPECT_EQ(Record(true, 1, &early), direct_one);
    EXPECT_EQ(early, 0u);

    // Enough rounds to cross the hand-over size several times: replay starts during recording
    // and the full trace still matches.
    const std::string direct = Record(false, 200, nullptr);
    const std::string deferred = Record(true, 200, &early);
    EXPECT_GT(early, 0u);
    EXPECT_LT(early, deferred.size());
    ASSERT_EQ(deferred.size(), direct.size());
    EXPECT_TRUE(deferred == direct);

    dispatcher = saved;
}

// A recorder with nothing pending accepts a new target; a direct one never holds records.
TEST(CommandRecorder, DirectRecorderHoldsNothing) {
    vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
    const vk::detail::DispatchLoaderDynamic saved = dispatcher;
    InstallFakes(dispatcher);
    std::string out;
    trace = &out;
    Vulkan::CommandRecorder recorder{false};
    recorder.SetTarget(Handle<vk::CommandBuffer>(1));
    const Vulkan::DeferredCommandBuffer cmd{&recorder};
    cmd.draw(1, 2, 3, 4);
    EXPECT_EQ(recorder.PendingRecords(), 0u);
    EXPECT_FALSE(out.empty());
    EXPECT_EQ(cmd.Direct(), Handle<vk::CommandBuffer>(1));
    recorder.SetTarget(Handle<vk::CommandBuffer>(2));
    EXPECT_EQ(cmd.Direct(), Handle<vk::CommandBuffer>(2));
    trace = nullptr;
    dispatcher = saved;
}

}  // namespace
