// The checks of the draw recorder that fail a draw before it records anything
// (docs/design/graphics.md, item 27). The rendering tests need a render fixture and come with
// the pipeline-level tests.
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "gpu/vk/device.hpp"
#include "gpu/vk/dispatcher.hpp"
#include "gpu/vk/draw_recorder.hpp"
#include "gpu/vk/image_cache.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "gpu/vk/pipeline_cache.hpp"
#include "gpu/vk/rendering.hpp"
#include "gpu/vk/scheduler.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"
#include "si_ci_vi_merged_offset.h"
#include "test_device.hpp"

namespace {

using namespace bb::vk;
using bb::hle::kernel::dmem::fd;
using bb::pm4::DrawCall;
using bb::pm4::DrawKind;
using bb::pm4::RegisterMirror;
using bb::pm4::RegSpace;
namespace amd = Pal::Gfx6::Chip;

void set(RegisterMirror& regs, std::uint32_t reg, std::uint32_t value) {
    for (const auto space : {RegSpace::sh, RegSpace::context, RegSpace::uconfig}) {
        const auto range = bb::pm4::space_range(space);
        if (reg >= range.base && reg < range.base + range.size) {
            regs.write(space, reg - range.base, {&value, 1});
        }
    }
}

// A triangle strip with restart enabled, in DX clip space (as in pipeline_state_test.cpp).
RegisterMirror strip_registers(std::uint32_t reset_index) {
    RegisterMirror regs;
    set(regs, amd::mmVGT_PRIMITIVE_TYPE__CI__VI, 6u);
    set(regs, amd::mmVGT_MULTI_PRIM_IB_RESET_EN, 1u);
    set(regs, amd::mmVGT_MULTI_PRIM_IB_RESET_INDX, reset_index);
    set(regs, amd::mmPA_CL_CLIP_CNTL, 1u << 19);
    set(regs, amd::mmPA_CL_VTE_CNTL, 0x43fu);
    set(regs, amd::mmPA_CL_VPORT_XSCALE, std::bit_cast<std::uint32_t>(960.0f));
    set(regs, amd::mmPA_CL_VPORT_YSCALE, std::bit_cast<std::uint32_t>(-540.0f));
    set(regs, amd::mmPA_CL_VPORT_ZSCALE, std::bit_cast<std::uint32_t>(1.0f));
    return regs;
}

class VkDrawRecorderUnit : public testing::Test {
protected:
    void SetUp() override {
        const std::string selector = bb::test::vk_test_device();
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = selector}, why);
        if (!device_) {
            GTEST_SKIP() << "no Vulkan device: " << why;
        }
        ASSERT_GE(fd(), 0);
        mirror_ = MemoryMirror::create(*device_,
                                       {.memfd = fd(),
                                        .window_start = bb::hle::kernel::vmem::window_start,
                                        .size = mirror_buffer_size,
                                        .min_block = 64 * 1024},
                                       why);
        if (!mirror_) {
            GTEST_SKIP() << "no mirror: " << why;
        }
        const CodeReader none = [](std::uint64_t) { return std::span<const std::uint32_t>(); };
        scheduler_ = std::make_unique<Scheduler>(*device_);
        compute_ = std::make_unique<ComputePipelineCache>(*device_, none);
        dispatcher_ = std::make_unique<Dispatcher>(*device_, *scheduler_, *compute_, *mirror_);
        images_ = std::make_unique<ImageCache>(*device_);
        targets_ = std::make_unique<TargetRenderer>(*device_, *images_);
        pipelines_ = std::make_unique<PipelineCache>(*device_, none);
        recorder_ = std::make_unique<DrawRecorder>(*device_, *scheduler_, *dispatcher_,
                                                   *pipelines_, *targets_, *mirror_, hints_);
    }
    void TearDown() override {
        recorder_.reset();
        dispatcher_.reset();  // waits for the GPU
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<MemoryMirror> mirror_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<ComputePipelineCache> compute_;
    std::unique_ptr<Dispatcher> dispatcher_;
    std::unique_ptr<ImageCache> images_;
    std::unique_ptr<TargetRenderer> targets_;
    std::unique_ptr<PipelineCache> pipelines_;
    bb::pm4::ExtentHints hints_;
    std::unique_ptr<DrawRecorder> recorder_;
};

TEST_F(VkDrawRecorderUnit, RestartNeedsTheAllOnesIndexOfTheIndexType) {
    // Index 16-bit with the 32-bit all-ones value and the other way round: not supported, and
    // rejected before anything is read from the (absent) index buffer.
    DrawCall call;
    call.kind = DrawKind::index_2;
    call.index_count = 3;
    call.index_address = bb::hle::kernel::vmem::window_start;
    call.index_bytes = 2;
    recorder_->draw(strip_registers(0xffffffffu), call);
    call.index_bytes = 4;
    recorder_->draw(strip_registers(0xffffu), call);
    recorder_->draw(strip_registers(0x1234u), call);  // the snapshot itself rejects this one
    EXPECT_EQ(recorder_->stats().unsupported, 3u);
    EXPECT_EQ(recorder_->stats().recorded, 0u);
    EXPECT_EQ(recorder_->stats().failed, 0u);
}

TEST_F(VkDrawRecorderUnit, IndirectDrawsAreNotSupportedYet) {
    DrawCall call;
    call.indirect = 0x1000;
    recorder_->draw(RegisterMirror{}, call);
    EXPECT_EQ(recorder_->stats().unsupported, 1u);
}

TEST_F(VkDrawRecorderUnit, AnIndexBufferOutsideTheMirrorFailsTheDraw) {
    DrawCall call;
    call.kind = DrawKind::index_2;
    call.index_count = 3;
    call.index_bytes = 2;
    call.index_address = bb::hle::kernel::vmem::window_start - 2;
    recorder_->draw(strip_registers(0xffffu), call);
    call.index_address = bb::hle::kernel::vmem::window_start + 1;  // unaligned
    recorder_->draw(strip_registers(0xffffu), call);
    EXPECT_EQ(recorder_->stats().failed, 2u);
    EXPECT_EQ(recorder_->stats().recorded, 0u);
}

}  // namespace
