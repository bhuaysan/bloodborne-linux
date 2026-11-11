#include "hle/video/video.hpp"

#include <sys/mman.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/equeue.hpp"
#include "hle/kernel/vmem.hpp"
#include "hle/video/gnm_shader.hpp"
#include "hle/video/video_out.hpp"
#include "loader/image.hpp"
#include "loader/relocate.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::hle::video;
using bb::hle::GuestEvent;
using namespace std::chrono_literals;

// Waits on the queue like 0x26d6f1b: capacity 1, a 4-byte count, the given timeout.
struct WaitOutcome {
    std::int32_t result = 0;
    std::int32_t count = -1;
    GuestEvent event;
};
WaitOutcome wait_for(std::uint64_t queue, const std::uint32_t* timeout_us) {
    WaitOutcome w;
    w.result = bb::hle::kernel::equeue_wait(queue, &w.event, 1, &w.count, timeout_us);
    return w;
}

// The whole 64-byte status block is written.
FlipStatus flip_status(std::int32_t port) {
    std::array<unsigned char, sizeof(FlipStatus) + 8> block;
    block.fill(0xa5);
    EXPECT_EQ(sceVideoOutGetFlipStatus(port, block.data()), 0);
    for (std::size_t i = sizeof(FlipStatus); i < block.size(); ++i) {
        EXPECT_EQ(block[i], 0xa5) << i;  // nothing past the block
    }
    FlipStatus status;
    std::memcpy(&status, block.data(), sizeof status);
    EXPECT_EQ(status.reserved0, 0u);
    EXPECT_EQ(status.reserved1, 0u);
    return status;
}

std::uint64_t flip_count(std::int32_t port) {
    return flip_status(port).count;
}

// The flip argument of a flip event (bits 16 and up of its data).
std::int64_t event_flip_arg(const GuestEvent& event) {
    return event.data >> 16;
}

// A port set up as at 0x11e488e..0x11e4915: open, flip queue, two 1920x1080 buffers.
struct Port {
    Port() {
        handle = sceVideoOutOpen(0, 0, 0, nullptr);
        EXPECT_EQ(handle, main_port_handle);
        EXPECT_EQ(bb::hle::kernel::equeue_create(&queue, "FlipEventQueue"), 0);
        EXPECT_EQ(sceVideoOutAddFlipEvent(queue, handle, 0x1234), 0);
        std::array<unsigned char, 0x78> attr{};
        EXPECT_EQ(sceVideoOutSetBufferAttribute(attr.data(), 0x80000000, 0, 0, 1920, 1080, 1920),
                  0);
        EXPECT_EQ(sceVideoOutRegisterBuffers(handle, 0, buffers.data(), 2, attr.data()), 0);
    }
    ~Port() {
        EXPECT_EQ(sceVideoOutClose(handle), 0);
        EXPECT_EQ(bb::hle::kernel::equeue_delete(queue), 0);
    }
    static constexpr std::array<std::uint64_t, 2> buffers{0x10000000, 0x10800000};
    std::int32_t handle = 0;
    std::uint64_t queue = 0;
};

// Upstream has one port: a second open is busy until the first is closed, and the handle is 1
// again after that.
TEST(HleVideo, OnlyOnePortOpensAtATime) {
    const std::int32_t a = sceVideoOutOpen(0, 0, 0, nullptr);
    EXPECT_EQ(a, main_port_handle);
    EXPECT_EQ(sceVideoOutOpen(0, 0, 0, nullptr), video_out_resource_busy);
    EXPECT_EQ(sceVideoOutSetFlipRate(a, 0), 0);
    EXPECT_EQ(sceVideoOutSetFlipRate(a + 1, 0), video_out_invalid_handle);
    EXPECT_EQ(sceVideoOutClose(a), 0);
    EXPECT_EQ(sceVideoOutClose(a + 1), video_out_invalid_handle);
    EXPECT_EQ(sceVideoOutOpen(0, 5, 0, nullptr), video_out_invalid_value);  // not the main bus
    EXPECT_EQ(sceVideoOutOpen(0, 0, 1, nullptr), video_out_invalid_value);
    EXPECT_EQ(sceVideoOutOpen(0, 0, 0, nullptr), a);
    EXPECT_EQ(sceVideoOutClose(a), 0);
}

TEST(HleVideo, CallsOnUnknownPortsFail) {
    std::uint64_t queue = 0;
    ASSERT_EQ(bb::hle::kernel::equeue_create(&queue, nullptr), 0);
    std::array<unsigned char, 0x40> block{};
    const std::uint64_t buffer = 0x10000000;
    EXPECT_LT(sceVideoOutAddFlipEvent(queue, 0x7fff0000, 0), 0);
    EXPECT_LT(sceVideoOutRegisterBuffers(0x7fff0000, 0, &buffer, 1, block.data()), 0);
    EXPECT_LT(sceVideoOutSubmitFlip(0x7fff0000, 0, 1, 0), 0);
    EXPECT_LT(sceVideoOutGetFlipStatus(0x7fff0000, block.data()), 0);
    // Registering on the port before it is open.
    EXPECT_EQ(sceVideoOutRegisterBuffers(main_port_handle, 0, &buffer, 1, block.data()),
              video_out_invalid_handle);
    const std::int32_t port = sceVideoOutOpen(0, 0, 0, nullptr);
    EXPECT_EQ(sceVideoOutAddFlipEvent(queue + 1000, port, 0), video_out_invalid_event_queue);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, max_display_buffers - 1, &buffer, 2, block.data()),
              video_out_invalid_value);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, -1, block.data()),
              video_out_invalid_value);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, -1, &buffer, 1, block.data()),
              video_out_invalid_value);
    // A flip to a slot without a buffer, or out of range.
    EXPECT_EQ(sceVideoOutSubmitFlip(port, 0, 1, 0), video_out_invalid_index);
    EXPECT_EQ(sceVideoOutSubmitFlip(port, max_display_buffers, 1, 0), video_out_invalid_index);
    EXPECT_EQ(sceVideoOutSubmitFlip(port, -2, 1, 0), video_out_invalid_index);
    EXPECT_EQ(flip_status(port).flip_pending_num, 0);
    EXPECT_EQ(sceVideoOutClose(port), 0);
    EXPECT_EQ(bb::hle::kernel::equeue_delete(queue), 0);
}

// 0x26d65d6 registers with count 0 when the configured buffer count is 0 and fails on a
// negative result, so count 0 succeeds on an open port: it takes an attribute group. Once the
// four groups are taken, the next registration fails; closing frees them.
TEST(HleVideo, RegisteringNoBuffersTakesAGroup) {
    std::array<unsigned char, 0x40> block{};
    const std::uint64_t buffer = 0x10000000;
    const std::int32_t port = sceVideoOutOpen(0, 0, 0, nullptr);
    for (std::int32_t group = 0; group < 4; ++group) {
        EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 0, block.data()), group);
    }
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 0, block.data()),
              video_out_no_empty_slot);
    EXPECT_EQ(sceVideoOutClose(port), 0);
    EXPECT_LT(sceVideoOutRegisterBuffers(port, 0, &buffer, 0, block.data()), 0);  // closed
    EXPECT_EQ(sceVideoOutOpen(0, 0, 0, nullptr), port);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 0, block.data()), 0);
    EXPECT_EQ(sceVideoOutClose(port), 0);
}

// Registration checks the attribute as upstream does, and a slot holds one buffer.
TEST(HleVideo, RegistrationChecksTheAttributeAndTheSlots) {
    const std::int32_t port = sceVideoOutOpen(0, 0, 0, nullptr);
    const std::uint64_t buffer = 0x10000000;
    std::array<unsigned char, 0x40> attr{};
    ASSERT_EQ(sceVideoOutSetBufferAttribute(attr.data(), 0x80000000, 0, 1, 1920, 1080, 1920), 0);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 1, attr.data()),
              video_out_invalid_aspect_ratio);
    ASSERT_EQ(sceVideoOutSetBufferAttribute(attr.data(), 0x80000000, 0, 0, 1920, 1080, 1280), 0);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 1, attr.data()),
              video_out_invalid_pitch);
    ASSERT_EQ(sceVideoOutSetBufferAttribute(attr.data(), 0x80000000, 2, 0, 1920, 1080, 1920), 0);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 1, attr.data()),
              video_out_invalid_tiling_mode);
    ASSERT_EQ(sceVideoOutSetBufferAttribute(attr.data(), 0x80000000, 1, 0, 1920, 1080, 1920), 0);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 1, attr.data()), 0);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 1, attr.data()),
              video_out_slot_occupied);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 1, &buffer, 1, attr.data()), 1);
    EXPECT_EQ(sceVideoOutClose(port), 0);
}

TEST(HleVideo, BufferAttributeFillsOnlyTheBlock) {
    std::array<unsigned char, 0x40> block;
    block.fill(0xa5);
    EXPECT_EQ(sceVideoOutSetBufferAttribute(block.data() + 1, 0x80000000, 1, 0, 1920, 1080, 1920),
              0);
    BufferAttribute a;
    static_assert(sizeof a == 40);
    std::memcpy(&a, block.data() + 1, sizeof a);
    EXPECT_EQ(static_cast<std::uint32_t>(a.pixel_format), 0x80000000u);
    EXPECT_EQ(static_cast<std::int32_t>(a.tiling_mode), 1);
    EXPECT_EQ(a.aspect_ratio, 0);
    EXPECT_EQ(a.width, 1920u);
    EXPECT_EQ(a.height, 1080u);
    EXPECT_EQ(a.pitch_in_pixel, 1920u);
    EXPECT_EQ(a.option, 0u);
    EXPECT_EQ(a.reserved0, 0u);
    EXPECT_EQ(a.reserved1, 0u);
    EXPECT_EQ(block[0], 0xa5);
    for (std::size_t i = 1 + sizeof a; i < block.size(); ++i) {
        EXPECT_EQ(block[i], 0xa5) << i;
    }
}

TEST(HleVideo, FlipWakesAWaiterWithTheFlipEvent) {
    const Port port;
    EXPECT_EQ(flip_count(port.handle), 0u);
    const auto queue = bb::hle::EventQueue::find(port.queue);
    ASSERT_NE(queue, nullptr);
    WaitOutcome outcome;
    std::thread waiter([&] { outcome = wait_for(port.queue, nullptr); });  // as at 0x11e41b6
    // Submit only once the waiter is blocked, so the flip has to wake it.
    while (queue->waiting() == 0) {
        std::this_thread::yield();
    }
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 1, 2, 77), 0);
    waiter.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(outcome.result, 0);
    EXPECT_EQ(outcome.count, 1);
    EXPECT_EQ(outcome.event.id, flip_event_id);
    EXPECT_EQ(outcome.event.kind, video_out_filter);
    EXPECT_EQ(outcome.event.user_data, 0x1234u);
    EXPECT_EQ(event_flip_arg(outcome.event), 77);
    EXPECT_EQ((outcome.event.data >> 12) & 0xf, 1);  // the count of the event
    const FlipStatus status = flip_status(port.handle);
    EXPECT_EQ(status.count, 1u);
    EXPECT_EQ(status.flip_arg, 77);  // what 0x26d6f06 compares
    EXPECT_EQ(status.current_buffer, 1);
    EXPECT_EQ(status.flip_pending_num, 0);
    EXPECT_EQ(status.gc_queue_num, 0);
    EXPECT_GT(status.submit_tsc, 0u);
    EXPECT_GE(status.tsc, status.submit_tsc);
    EXPECT_GT(status.process_time, 0u);
    EXPECT_LE(elapsed, vblank_period + 200ms);  // the next vblank, with room for a loaded machine
}

// A flip to index -1 shows a blank frame; it completes like any other.
TEST(HleVideo, BlankFlipCompletes) {
    const Port port;
    EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, -1, 1, 9), 0);
    const WaitOutcome w = wait_for(port.queue, nullptr);
    EXPECT_EQ(w.result, 0);
    EXPECT_EQ(event_flip_arg(w.event), 9);
    EXPECT_EQ(flip_status(port.handle).current_buffer, -1);
}

// A presenter that records what video out asks of it. send_command runs the work at once.
struct FakePresenter final : FlipPresenter {
    void set_vo_port(VideoOutPort* p) override { port = p; }
    void send_command(std::function<void()> work) override {
        ++commands;
        work();
    }
    void register_surface(const BufferAttributeGroup& group, std::uint64_t address) override {
        const std::scoped_lock lock{mutex};
        surfaces.push_back(address);
        surface_width = group.attrib.width;
    }
    Vulkan::Frame* prepare_frame(const BufferAttributeGroup& group,
                                 std::uint64_t address) override {
        const std::scoped_lock lock{mutex};
        prepared.push_back(address);
        prepared_width = group.attrib.width;
        return game_frame();
    }
    Vulkan::Frame* prepare_blank_frame(bool) override { return blank_frame(); }
    Vulkan::Frame* prepare_last_frame() override { return nullptr; }
    void present(Vulkan::Frame* frame, bool reusing, bool is_game_frame) override {
        if (frame == blank_frame()) {
            EXPECT_FALSE(is_game_frame);
            ++blank_presents;
            return;
        }
        EXPECT_EQ(frame, game_frame());
        EXPECT_FALSE(reusing);
        EXPECT_TRUE(is_game_frame);
        // Presented before the flip counts and its event goes out.
        count_at_present = flip_count(main_port_handle);
        ++game_presents;
    }
    void set_hdr(bool enable) override { EXPECT_FALSE(enable); }
    bool must_keep_drawing() override { return false; }

    static Vulkan::Frame* game_frame() { return reinterpret_cast<Vulkan::Frame*>(0x1000); }
    static Vulkan::Frame* blank_frame() { return reinterpret_cast<Vulkan::Frame*>(0x2000); }

    std::mutex mutex;
    std::atomic<VideoOutPort*> port{nullptr};
    std::atomic<int> commands{0};
    std::vector<std::uint64_t> surfaces;
    std::vector<std::uint64_t> prepared;
    std::uint32_t surface_width = 0;
    std::uint32_t prepared_width = 0;
    std::atomic<int> game_presents{0};
    std::atomic<int> blank_presents{0};
    std::atomic<std::uint64_t> count_at_present{~0ull};
};

// With a presenter, the port goes to Liverpool at the open, its buffers become video out
// surfaces, and a flip prepares its frame on the GPU thread and presents it before the event.
// While the port is closed the present thread shows blank frames.
TEST(HleVideo, PresenterPresentsTheFlipBeforeTheEvent) {
    static FakePresenter fake;  // the present thread may still hold it after the test
    start_video_out(&fake);
    {
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (fake.blank_presents == 0 && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_GT(fake.blank_presents, 0);
        const Port port;
        ASSERT_NE(fake.port.load(), nullptr);
        {
            const std::scoped_lock lock{fake.mutex};
            EXPECT_EQ(fake.surfaces, (std::vector<std::uint64_t>{Port::buffers.begin(),
                                                                 Port::buffers.end()}));
            EXPECT_EQ(fake.surface_width, 1920u);
        }
        EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 1, 1, 5), 0);
        const std::uint32_t timeout_us = 1'000'000;
        const WaitOutcome outcome = wait_for(port.queue, &timeout_us);
        EXPECT_EQ(outcome.result, 0);
        EXPECT_EQ(fake.commands, 1);
        EXPECT_EQ(fake.game_presents, 1);
        EXPECT_EQ(fake.count_at_present, 0u);
        const std::scoped_lock lock{fake.mutex};
        EXPECT_EQ(fake.prepared, (std::vector<std::uint64_t>{Port::buffers[1]}));
        EXPECT_EQ(fake.prepared_width, 1920u);
        start_video_out(nullptr);
    }
}

// The flip labels Liverpool waits on: a completed flip clears the label of the buffer shown
// before it, and registration clears the labels of the new buffers.
TEST(HleVideo, FlipClearsTheLabelOfThePreviousBuffer) {
    static FakePresenter fake;
    start_video_out(&fake);
    const Port port;
    VideoOutPort* const vo = fake.port;
    ASSERT_NE(vo, nullptr);
    start_video_out(nullptr);
    EXPECT_TRUE(vo->IsVoLabel(&vo->buffer_labels[1]));
    EXPECT_EQ(vo->buffer_labels[0], 0u);
    vo->buffer_labels[0] = 1;  // as the GPU writes them before a flip
    vo->buffer_labels[1] = 1;
    EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 1, 1, 0), 0);
    ASSERT_EQ(wait_for(port.queue, nullptr).result, 0);
    EXPECT_EQ(vo->buffer_labels[0], 1u);  // no flip before the first
    EXPECT_EQ(vo->buffer_labels[1], 1u);
    EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 0, 1, 0), 0);
    ASSERT_EQ(wait_for(port.queue, nullptr).result, 0);
    EXPECT_EQ(vo->buffer_labels[0], 1u);
    EXPECT_EQ(vo->buffer_labels[1], 0u);  // shown before the flip to buffer 0
}

TEST(HleVideo, FlipsCompleteOnePerVblank) {
    const Port port;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, i % 2, 1, 0), 0);
    }
    // The wait loop of 0x26d6ee0..0x26d6f2d: wait for events until the count has caught up.
    while (flip_count(port.handle) < 3) {
        const WaitOutcome w = wait_for(port.queue, nullptr);
        ASSERT_EQ(w.result, 0);
    }
    // A flip completes at the first vblank after it was queued, so the third one needs two more
    // vblanks after the first.
    EXPECT_GE(std::chrono::steady_clock::now() - start, 2 * vblank_period);
    EXPECT_EQ(flip_count(port.handle), 3u);
}

// With flip rate 1 the present thread takes a flip at every second vblank.
TEST(HleVideo, FlipRateSkipsVblanks) {
    const Port port;
    ASSERT_EQ(sceVideoOutSetFlipRate(port.handle, 1), 0);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, i % 2, 1, 0), 0);
    }
    while (flip_count(port.handle) < 3) {
        ASSERT_EQ(wait_for(port.queue, nullptr).result, 0);
    }
    EXPECT_GE(std::chrono::steady_clock::now() - start, 4 * vblank_period);
}

TEST(HleVideo, FlipReachesOnlyRegisteredQueues) {
    const Port port;
    std::uint64_t other = 0;
    ASSERT_EQ(bb::hle::kernel::equeue_create(&other, nullptr), 0);
    EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 0, 1, 0), 0);
    const WaitOutcome w = wait_for(port.queue, nullptr);
    EXPECT_EQ(w.event.id, flip_event_id);
    const std::uint32_t timeout = 50'000;
    EXPECT_LT(wait_for(other, &timeout).result, 0);  // timed out
    EXPECT_EQ(bb::hle::kernel::equeue_delete(other), 0);
}

TEST(HleVideo, GnmSubmitsSucceedAndSubmitAndFlipFlips) {
    const std::array<std::uint64_t, 2> buffers{0x20000000, 0x20010000};  // unmapped: left out
    const std::array<std::uint32_t, 2> sizes{0x100, 0x40};
    EXPECT_EQ(gnm_submit(2, buffers.data(), sizes.data(), nullptr, nullptr), 0);
    EXPECT_EQ(gnm_submit_other(1, buffers.data(), sizes.data(), nullptr, nullptr),
              validation_not_enabled);

    const Port port;
    EXPECT_EQ(gnm_submit_and_flip(1, buffers.data(), sizes.data(), nullptr, nullptr, port.handle,
                                  1, 1, 5),
              0);
    EXPECT_EQ(sceGnmSubmitDone(), 0);
    const WaitOutcome w = wait_for(port.queue, nullptr);
    EXPECT_EQ(event_flip_arg(w.event), 5);
    EXPECT_EQ(flip_count(port.handle), 1u);
    EXPECT_EQ(flip_status(port.handle).current_buffer, 1);
}

// Command buffers in guest memory run on the GPU thread; validation queues nothing.
TEST(HleVideo, GnmSubmitsQueueGuestBuffersOnTheGpuThread) {
    namespace vmem = bb::hle::kernel::vmem;
    vmem::Mapping m;
    m.length = vmem::page_size;
    m.prot = PROT_READ | PROT_WRITE;
    m.host_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    std::uintptr_t base = 0;
    ASSERT_EQ(vmem::map(m, base), 0);
    auto* dwords = reinterpret_cast<std::uint32_t*>(base);
    // Three streams, each one SET_CONTEXT_REG of 0xA000 + offset (header, offset, value).
    const std::array<std::array<std::uint32_t, 3>, 3> streams{{
        {bb::pm4::type3_header(IT_SET_CONTEXT_REG, 2), 0x300, 1},
        {bb::pm4::type3_header(IT_SET_CONTEXT_REG, 2), 0x301, 2},
        {bb::pm4::type3_header(IT_SET_CONTEXT_REG, 2), 0x300, 3},
    }};
    std::memcpy(dwords, streams.data(), sizeof streams);
    const std::array<std::uint64_t, 3> buffers{base, base + 12, base + 24};
    const std::array<std::uint32_t, 3> sizes{12, 12, 12};

    ASSERT_TRUE(gpu_thread().wait_idle(5s));
    const bb::pm4::GpuThread::Stats before = gpu_thread().stats();
    EXPECT_EQ(gnm_submit_other(1, &buffers[2], &sizes[2], nullptr, nullptr),
              validation_not_enabled);
    EXPECT_EQ(gnm_submit(2, buffers.data(), sizes.data(), nullptr, nullptr), 0);
    ASSERT_TRUE(gpu_thread().wait_idle(5s));
    EXPECT_EQ(gpu_thread().registers().get(0xA300), 1u);
    EXPECT_EQ(gpu_thread().registers().get(0xA301), 2u);

    const Port port;
    EXPECT_EQ(gnm_submit_and_flip(1, &buffers[2], &sizes[2], nullptr, nullptr, port.handle, 0, 1,
                                  0),
              0);
    ASSERT_TRUE(gpu_thread().wait_idle(5s));
    EXPECT_EQ(gpu_thread().registers().get(0xA300), 3u);
    const bb::pm4::GpuThread::Stats after = gpu_thread().stats();
    EXPECT_EQ(after.submitted - before.submitted, 3u);
    EXPECT_EQ(after.errors, before.errors);
    EXPECT_EQ(vmem::unmap(base, vmem::page_size), 0);
}

// A CCB runs on the constant queue beside its DCB; a pair with an unreadable buffer is left out.
TEST(HleVideo, GnmSubmitsRunConstantCommandBuffers) {
    namespace vmem = bb::hle::kernel::vmem;
    vmem::Mapping m;
    m.length = vmem::page_size;
    m.prot = PROT_READ | PROT_WRITE;
    m.host_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    std::uintptr_t base = 0;
    ASSERT_EQ(vmem::map(m, base), 0);
    auto* dwords = reinterpret_cast<std::uint32_t*>(base);
    const auto out = base + 0x100;  // where the CCB dumps CE RAM
    // DCB (7 dwords): WAIT_ON_CE_COUNTER, SET_CONTEXT_REG 0xA302 = 4, INCREMENT_DE_COUNTER.
    // CCB (10 dwords): WRITE_CONST_RAM, DUMP_CONST_RAM to `out`, INCREMENT_CE_COUNTER.
    const std::array<std::uint32_t, 17> streams{
        bb::pm4::type3_header(IT_WAIT_ON_CE_COUNTER, 1), 1,
        bb::pm4::type3_header(IT_SET_CONTEXT_REG, 2), 0x302, 4,
        bb::pm4::type3_header(IT_INCREMENT_DE_COUNTER, 1), 1,
        bb::pm4::type3_header(IT_WRITE_CONST_RAM, 2), 0x20, 0x5A,
        bb::pm4::type3_header(IT_DUMP_CONST_RAM, 4), 0x20, 1, static_cast<std::uint32_t>(out),
        static_cast<std::uint32_t>(out >> 32),
        bb::pm4::type3_header(IT_INCREMENT_CE_COUNTER, 1), 1};
    std::memcpy(dwords, streams.data(), sizeof streams);
    const std::array<std::uint64_t, 2> draw{base, base};
    const std::array<std::uint32_t, 2> draw_sizes{28, 28};
    const std::array<std::uint64_t, 2> ccb{base + 28, 0x10};  // the second is unmapped
    const std::array<std::uint32_t, 2> ccb_sizes{40, 40};

    ASSERT_TRUE(gpu_thread().wait_idle(5s));
    const bb::pm4::GpuThread::Stats before = gpu_thread().stats();
    EXPECT_EQ(gnm_submit(2, draw.data(), draw_sizes.data(), ccb.data(), ccb_sizes.data()), 0);
    ASSERT_TRUE(gpu_thread().wait_idle(5s));
    EXPECT_EQ(gpu_thread().registers().get(0xA302), 4u);
    EXPECT_EQ(dwords[0x40], 0x5Au);
    const bb::pm4::GpuThread::Stats after = gpu_thread().stats();
    EXPECT_EQ(after.submitted - before.submitted, 2u);
    EXPECT_EQ(after.errors, before.errors);
    EXPECT_EQ(vmem::unmap(base, vmem::page_size), 0);
}

// A map checks its arguments as shadPS4 does and hands out positive ids (the game treats 0 as a
// failed map, 0x108c7b0); a doorbell runs the ring and the read offset comes back.
TEST(HleVideo, ComputeQueuesRunTheirRing) {
    namespace vmem = bb::hle::kernel::vmem;
    vmem::Mapping m;
    m.length = vmem::page_size;
    m.prot = PROT_READ | PROT_WRITE;
    m.host_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    std::uintptr_t base = 0;
    ASSERT_EQ(vmem::map(m, base), 0);
    auto* ring = reinterpret_cast<std::uint32_t*>(base);
    auto* read_pointer = ring + 0x100;
    *read_pointer = 7;
    EXPECT_EQ(gnm_map_compute_queue(7, 0, ring, 0x40, read_pointer), compute_queue_invalid_pipe);
    EXPECT_EQ(gnm_map_compute_queue(0, 8, ring, 0x40, read_pointer), compute_queue_invalid_queue);
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring, 0x30, read_pointer), compute_queue_invalid_size);
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring + 1, 0x40, read_pointer),
              compute_queue_invalid_ring);
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring + 1, 0x30, read_pointer),
              compute_queue_invalid_ring);  // alignment is checked before the size
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring, 0x40, reinterpret_cast<std::uint32_t*>(0x10)),
              compute_queue_invalid_read_pointer);
    const std::int32_t first = gnm_map_compute_queue(0, 0, ring, 0x40, read_pointer);
    const std::int32_t second = gnm_map_compute_queue(1, 0, ring, 0x40, read_pointer);
    EXPECT_GT(first, 0);
    EXPECT_GT(second, first);
    EXPECT_EQ(*read_pointer, 0u);

    const std::array<std::uint32_t, 4> set{bb::pm4::type3_header(IT_SET_CONTEXT_REG, 3), 0x306,
                                           6, 0};
    std::memcpy(ring, set.data(), sizeof set);
    gnm_ding_dong(first, 4);
    gnm_ding_dong(0, 4);  // logged and ignored
    ASSERT_TRUE(gpu_thread().wait_idle(5s));
    EXPECT_EQ(gpu_thread().registers().get(0xA306), 6u);
    EXPECT_EQ(std::atomic_ref(*read_pointer).load(), 4u);
    EXPECT_EQ(gnm_unmap_compute_queue(first), 0);
    EXPECT_EQ(gnm_unmap_compute_queue(second), 0);
    EXPECT_EQ(gnm_unmap_compute_queue(first), 0);  // unknown: logged
    EXPECT_EQ(vmem::unmap(base, vmem::page_size), 0);
}

// An EVENT_WRITE_EOP with an interrupt writes its label and wakes the queue registered for it.
TEST(HleVideo, EndOfPipeInterruptTriggersGnmEvents) {
    namespace vmem = bb::hle::kernel::vmem;
    vmem::Mapping m;
    m.length = vmem::page_size;
    m.prot = PROT_READ | PROT_WRITE;
    m.host_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    std::uintptr_t base = 0;
    ASSERT_EQ(vmem::map(m, base), 0);
    auto* dwords = reinterpret_cast<std::uint32_t*>(base);
    const auto label = base + 0x100;
    const std::array<std::uint32_t, 6> eop{bb::pm4::type3_header(IT_EVENT_WRITE_EOP, 5), 0x504,
                                           static_cast<std::uint32_t>(label),
                                           1u << 29 | 2u << 24 |
                                               static_cast<std::uint32_t>(label >> 32),
                                           0x77, 0};
    std::memcpy(dwords, eop.data(), sizeof eop);
    const std::uint64_t buffer = base;
    const std::uint32_t size = sizeof eop;
    std::uint64_t queue = 0;
    ASSERT_EQ(bb::hle::kernel::equeue_create(&queue, "GnmEop"), 0);
    EXPECT_LT(sceGnmAddEqEvent(queue + 1000, bb::pm4::gfx_eop_interrupt, 0), 0);
    EXPECT_EQ(sceGnmAddEqEvent(queue, bb::pm4::gfx_eop_interrupt, 0x99), 0);

    EXPECT_EQ(gnm_submit(1, &buffer, &size, nullptr, nullptr), 0);
    const std::uint32_t timeout = 5'000'000;
    const WaitOutcome w = wait_for(queue, &timeout);
    EXPECT_EQ(w.result, 0);
    EXPECT_EQ(w.count, 1);
    EXPECT_EQ(w.event.kind, gnm_event_kind);
    EXPECT_EQ(w.event.user_data, 0x99u);
    EXPECT_EQ(sceGnmGetEqEventType(&w.event), 0x40);
    ASSERT_TRUE(gpu_thread().wait_idle(5s));
    EXPECT_EQ(dwords[0x40], 0x77u);

    // Without the registration the interrupt triggers nothing.
    EXPECT_EQ(sceGnmDeleteEqEvent(queue, bb::pm4::gfx_eop_interrupt), 0);
    EXPECT_EQ(gnm_submit(1, &buffer, &size, nullptr, nullptr), 0);
    ASSERT_TRUE(gpu_thread().wait_idle(5s));
    const std::uint32_t no_wait = 0;
    const auto timed_out = static_cast<std::int32_t>(0x8002003c);  // ETIMEDOUT
    EXPECT_EQ(wait_for(queue, &no_wait).result, timed_out);
    EXPECT_EQ(bb::hle::kernel::equeue_delete(queue), 0);
    EXPECT_EQ(vmem::unmap(base, vmem::page_size), 0);
}

// The eboot imports each of them under the library it is registered for, so relocating it puts
// the implementation into the GOT slot.
TEST(HleVideoGame, EbootImportsBindToTheImplementations) {
    BB_REQUIRE_GAME(game);
    bb::loader::Image image = bb::loader::map_eboot(game);
    bb::loader::relocate(image, bb::hle::resolve, bb::loader::eboot_module_id);

    struct Expected {
        std::string_view library, nid;
        void* impl;
    };
    const std::array<Expected, 23> expected{{
        {"libSceVideoOut", "Up36PTk687E", reinterpret_cast<void*>(&sceVideoOutOpen)},
        {"libSceGnmDriver", "29oKvKXzEZo", reinterpret_cast<void*>(&gnm_map_compute_queue)},
        {"libSceGnmDriver", "ArSg-TGinhk", reinterpret_cast<void*>(&gnm_unmap_compute_queue)},
        {"libSceGnmDriver", "bX5IbRvECXk", reinterpret_cast<void*>(&gnm_ding_dong)},
        {"libSceVideoOut", "uquVH4-Du78", reinterpret_cast<void*>(&sceVideoOutClose)},
        {"libSceVideoOut", "CBiu4mCE1DA", reinterpret_cast<void*>(&sceVideoOutSetFlipRate)},
        {"libSceVideoOut", "HXzjK9yI30k", reinterpret_cast<void*>(&sceVideoOutAddFlipEvent)},
        {"libSceVideoOut", "i6-sR91Wt-4", reinterpret_cast<void*>(&sceVideoOutSetBufferAttribute)},
        {"libSceVideoOut", "w3BY+tAEiQY", reinterpret_cast<void*>(&sceVideoOutRegisterBuffers)},
        {"libSceVideoOut", "U46NwOiJpys", reinterpret_cast<void*>(&sceVideoOutSubmitFlip)},
        {"libSceVideoOut", "SbU3dwp80lQ", reinterpret_cast<void*>(&sceVideoOutGetFlipStatus)},
        {"libSceGnmDriver", "zwY0YV91TTI", reinterpret_cast<void*>(&gnm_submit)},
        {"libSceGnmDriver", "iCO804ZgzdA", reinterpret_cast<void*>(&gnm_submit_other)},
        {"libSceGnmDriver", "xbxNatawohc", reinterpret_cast<void*>(&gnm_submit_and_flip)},
        {"libSceGnmDriver", "yvZ73uQUqrk", reinterpret_cast<void*>(&sceGnmSubmitDone)},
        {"libSceGnmDriver", "gAhCn6UiU4Y", reinterpret_cast<void*>(&sceGnmSetVsShader)},
        {"libSceGnmDriver", "V31V01UiScY", reinterpret_cast<void*>(&sceGnmSetVsShader)},
        {"libSceGnmDriver", "bQVd5YzCal0", reinterpret_cast<void*>(&sceGnmSetPsShader)},
        {"libSceGnmDriver", "4MgRw-bVNQU", reinterpret_cast<void*>(&sceGnmSetPsShader)},
        {"libSceGnmDriver", "KXltnCwEJHQ", reinterpret_cast<void*>(&sceGnmSetCsShader)},
        {"libSceGnmDriver", "b0xyllnVY-I", reinterpret_cast<void*>(&sceGnmAddEqEvent)},
        {"libSceGnmDriver", "PVT+fuoS9gU", reinterpret_cast<void*>(&sceGnmDeleteEqEvent)},
        {"libSceGnmDriver", "UoYY0DWMC0U", reinterpret_cast<void*>(&sceGnmGetEqEventType)},
    }};
    std::set<std::string> imports;
    for (const bb::loader::Symbol& sym : image.dyn.symbols) {
        if (sym.kind == bb::loader::Symbol::Kind::import) {
            imports.insert(bb::hle::import_key(sym.decoded.library, sym.decoded.nid));
        }
    }
    for (const Expected& e : expected) {
        SCOPED_TRACE(e.nid);
        // import_address aborts for an import that was never resolved.
        ASSERT_TRUE(imports.contains(bb::hle::import_key(e.library, e.nid)));
        EXPECT_EQ(bb::hle::import_address(e.library, e.nid),
                  reinterpret_cast<std::uintptr_t>(e.impl));
    }
}

}  // namespace
