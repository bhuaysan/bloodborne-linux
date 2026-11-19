// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/vk_scheduler.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-19
// License texts: third_party/shadps4/LICENSES/

#include "common/assert.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "common/frame_profile.hpp"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;
bool threaded_recording = true;

Scheduler::Scheduler(const Instance& instance, Recording recording_)
    : instance{instance}, recording{recording_}, recorder{recording_ != Recording::Direct},
      work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    BeginPrimary();
    dynamic_state.Invalidate();
    if (recording == Recording::Threaded) {
        recorder.SetHandOver([this](RecordedChunk&& chunk) {
            Work work{};
            work.chunk = std::move(chunk);
            Enqueue(std::move(work));
        });
        recording_thread = std::thread([this] { RecordingLoop(); });
    }
}

// The recording thread replays and submits what is queued before it stops, so every tick handed
// to it reaches the queue and a wait on it returns.
Scheduler::~Scheduler() {
    if (recording_thread.joinable()) {
        {
            std::scoped_lock lk{work_mutex};
            stop = true;
        }
        work_cv.notify_one();
        recording_thread.join();
        // The pool goes with the scheduler: its command buffers must be done.
        work_semaphore.Wait(work_semaphore.CurrentTick() - 1);
    }
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    CommandBuffer().beginRendering(rendering_info);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    CommandBuffer().endRendering();
    // The end of a render pass is a hand-over point (docs/design/command-recording.md).
    recorder.HandOver();
}

void Scheduler::ResetCommandBuffer() {
    recorder.HandOver();
    Drain();
    Check(primary.reset());
}

void Scheduler::Drain() {
    if (recording != Recording::Threaded) {
        return;
    }
    std::unique_lock lk{work_mutex};
    idle_cv.wait(lk, [this] { return work_queue.empty() && !working; });
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    bb::common::frame_profile::Scope profile_wait(bb::common::frame_profile::Part::wait);
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    work_semaphore.Wait(tick);
}

void Scheduler::WaitSubmitted(u64 tick) {
    if (recording != Recording::Threaded ||
        submitted_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    // Called from another thread, so it can't flush: the tick has to be flushed already.
    ASSERT_MSG(tick < work_semaphore.CurrentTick(), "waiting for tick {} that is not flushed",
               tick);
    std::unique_lock lk{submitted_mutex};
    submitted_cv.wait(lk, [&] { return submitted_tick.load(std::memory_order_acquire) >= tick; });
}

void Scheduler::PopPendingOperations() {
    // Every draw and dispatch calls this. The semaphore query is an ioctl, so it is made only when
    // the first operation was submitted and the last known GPU tick doesn't free it yet, and at
    // most once per call (#589). Operations still run in the order they were deferred.
    std::unique_lock lk(pending_ops_mutex);
    bool refreshed = false;
    while (!pending_ops.empty()) {
        const u64 tick = pending_ops.front().gpu_tick;
        if (!work_semaphore.IsFree(tick)) {
            // A tick at or past CurrentTick() has not been submitted, so the GPU can't be there.
            if (refreshed || tick >= work_semaphore.CurrentTick()) {
                break;
            }
            work_semaphore.Refresh();
            refreshed = true;
            if (!work_semaphore.IsFree(tick)) {
                break;
            }
        }
        pending_ops.front().callback();
        pending_ops.pop();
    }
}

void Scheduler::BeginPrimary() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    primary = command_pool.Commit();
    Check(primary.begin(begin_info));
    if (recording != Recording::Threaded) {
        recorder.SetTarget(primary);
    }
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    // The callbacks read and change GPU-thread state, so they run here and not on the recording
    // thread. The submit callback's sparse binds take submit_mutex themselves.
    if (on_submit) {
        on_submit(info);
    }
    if (on_session) {
        on_session();
    }
    EndRendering();
    recorder.HandOver();

    // The tick is assigned here: the GPU thread's lifetime rules (DeferOperation, IsFree) keep
    // working on it whether the submit happens now or on the recording thread.
    const u64 signal_value = work_semaphore.NextTick();
    info.AddSignal(work_semaphore.Handle(), signal_value);
    Work work{};
    work.submit = true;
    work.info = info;
    work.signal_value = signal_value;
    if (recording == Recording::Threaded) {
        Enqueue(std::move(work));
    } else {
        Process(work);
    }

    // Dynamic state goes into the next command buffer anew.
    dynamic_state.Invalidate();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::Enqueue(Work&& work) {
    {
        std::scoped_lock lk{work_mutex};
        work_queue.push_back(std::move(work));
    }
    work_cv.notify_one();
}

// Replays a chunk into the primary command buffer, or ends and submits it and begins the next.
// On the recording thread, or on the GPU thread without one.
void Scheduler::Process(Work& work) {
    if (work.chunk.data) {
        CommandRecorder::ReplayChunk(primary, work.chunk);
        recorder.Recycle(std::move(work.chunk));
    }
    if (!work.submit) {
        return;
    }
    if (recording != Recording::Threaded) {
        recorder.Replay();
    }
    Check(primary.end());

    SubmitInfo& info = work.info;
    std::array<vk::PipelineStageFlags, std::tuple_size_v<decltype(info.wait_semas)>>
        wait_stage_masks;
    wait_stage_masks.fill(vk::PipelineStageFlagBits::eAllCommands);

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1u,
        .pCommandBuffers = &primary,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    {
        std::scoped_lock lk{submit_mutex};
        auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
        ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    }
    if (recording == Recording::Threaded) {
        {
            std::scoped_lock lk{submitted_mutex};
            submitted_tick.store(work.signal_value, std::memory_order_release);
        }
        submitted_cv.notify_all();
    }

    work_semaphore.Refresh();
    BeginPrimary();
}

void Scheduler::RecordingLoop() {
    Common::SetCurrentThreadName("shadPS4:GpuRec");
    std::unique_lock lk{work_mutex};
    while (true) {
        work_cv.wait(lk, [this] { return stop || !work_queue.empty(); });
        if (work_queue.empty()) {
            return; // stop, with everything processed
        }
        Work work = std::move(work_queue.front());
        work_queue.pop_front();
        working = true;
        lk.unlock();
        Process(work);
        lk.lock();
        working = false;
        if (work_queue.empty()) {
            idle_cv.notify_all();
        }
    }
}

void DynamicState::Commit(const Instance& instance, const DeferredCommandBuffer& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
