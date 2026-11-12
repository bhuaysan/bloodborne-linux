#include "hle/video/video.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/platform.h"
#include "hle/hle.hpp"
#include "hle/kernel/equeue.hpp"
#include "hle/kernel/vmem.hpp"
#include "hle/video/gnm_driver.hpp"
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
    EXPECT_EQ(sceVideoOutSetFlipRate(a, max_flip_rate), 0);
    EXPECT_EQ(sceVideoOutSetFlipRate(a, max_flip_rate + 1), video_out_invalid_value);
    EXPECT_EQ(sceVideoOutSetFlipRate(a, 0x7fffffff), video_out_invalid_value);
    EXPECT_EQ(sceVideoOutSetFlipRate(a, -1), video_out_invalid_value);
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
    // start + count would overflow an int.
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0x7fffffff, &buffer, 1, block.data()),
              video_out_invalid_value);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 1, &buffer, 0x7fffffff, block.data()),
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

// A presenter that records what video out asks of it. send_command runs the work at once, or
// keeps it for run_deferred with `defer`. With `hold`, presenting a game frame waits until it is
// cleared.
struct FakePresenter final : FlipPresenter {
    void set_vo_port(VideoOutPort* p) override { port = p; }
    void send_command(std::function<void()> work) override {
        ++commands;
        if (defer) {
            const std::scoped_lock lock{mutex};
            deferred.push_back(std::move(work));
            return;
        }
        work();
    }
    void run_deferred() {
        std::vector<std::function<void()>> work;
        {
            const std::scoped_lock lock{mutex};
            work.swap(deferred);
        }
        for (const auto& w : work) {
            w();
        }
    }
    void register_surface(const BufferAttributeGroup& group, std::uint64_t address) override {
        // Video out calls this outside the port lock, so the port can be read meanwhile (under
        // the lock this would deadlock).
        EXPECT_EQ(flip_status(main_port_handle).count, 0u);
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
    Vulkan::Frame* prepare_blank_frame(bool present_thread) override {
        if (!present_thread) {
            ++gpu_blank_frames;
        }
        return blank_frame();
    }
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
        in_present = true;
        while (hold) {
            std::this_thread::sleep_for(1ms);
        }
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
    std::atomic<bool> defer{false};
    std::vector<std::function<void()>> deferred;
    std::atomic<int> gpu_blank_frames{0};
    std::atomic<bool> hold{false};
    std::atomic<bool> in_present{false};
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

// A flip the present thread is presenting while the port closes still gets its frame presented,
// but it no longer counts: the reopened port's first flip is flip 1 with no flip pending, and the
// flip from before the close is not its previous buffer, whose label it would clear.
TEST(HleVideo, FlipPresentedAcrossACloseLeavesTheNextPortAlone) {
    static FakePresenter fake;
    start_video_out(&fake);
    fake.hold = true;
    {
        const Port port;
        EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 1, 1, 5), 0);
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (!fake.in_present && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_TRUE(fake.in_present);
    }  // closed while the flip is presented
    const Port port;
    VideoOutPort* const vo = fake.port;
    ASSERT_NE(vo, nullptr);
    vo->buffer_labels[1] = 1;  // as the GPU writes it before a flip
    fake.hold = false;
    EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 0, 1, 6), 0);
    const std::uint32_t timeout_us = 1'000'000;
    const WaitOutcome outcome = wait_for(port.queue, &timeout_us);
    EXPECT_EQ(outcome.result, 0);
    EXPECT_EQ(event_flip_arg(outcome.event), 6);
    EXPECT_EQ(fake.game_presents, 2);
    const FlipStatus status = flip_status(port.handle);
    EXPECT_EQ(status.count, 1u);
    EXPECT_EQ(status.flip_pending_num, 0);
    EXPECT_EQ(status.flip_arg, 6);
    EXPECT_EQ(status.current_buffer, 0);
    EXPECT_EQ(vo->buffer_labels[1], 1u);
    start_video_out(nullptr);
}

// A blank flip that reaches the GPU thread only after the port closed is dropped there: no frame
// is prepared for it and the reopened port's status stays at zero.
TEST(HleVideo, BlankFlipQueuedAcrossACloseIsDropped) {
    static FakePresenter fake;
    start_video_out(&fake);
    fake.defer = true;
    {
        const Port port;
        EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, -1, 1, 5), 0);
        EXPECT_EQ(flip_status(port.handle).flip_pending_num, 1);
    }
    const Port port;
    fake.run_deferred();  // the GPU thread reaches the flip after the close
    fake.defer = false;
    EXPECT_EQ(fake.gpu_blank_frames, 0);
    EXPECT_EQ(fake.commands, 1);
    const FlipStatus closed = flip_status(port.handle);
    EXPECT_EQ(closed.count, 0u);
    EXPECT_EQ(closed.flip_pending_num, 0);
    EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 0, 1, 6), 0);
    const std::uint32_t timeout_us = 1'000'000;
    const WaitOutcome outcome = wait_for(port.queue, &timeout_us);
    EXPECT_EQ(outcome.result, 0);
    EXPECT_EQ(event_flip_arg(outcome.event), 6);
    const FlipStatus status = flip_status(port.handle);
    EXPECT_EQ(status.count, 1u);
    EXPECT_EQ(status.flip_pending_num, 0);
    start_video_out(nullptr);
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

// The flip of a GPU flip packet (sceVideoOutSubmitEopFlip): queued only when the GPU reaches the
// packet, which first sets the buffer's flip label, and signals GfxFlip. It is prepared on the
// signalling (GPU) thread rather than sent as a command, counts in gc_queue_num until it
// completes, and completes with its event.
TEST(HleVideo, EopFlipIsQueuedAtTheFlipInterrupt) {
    static FakePresenter fake;
    start_video_out(&fake);
    const Port port;
    std::uintptr_t labels = 0;
    EXPECT_EQ(get_buffer_label_address(port.handle, nullptr), video_out_invalid_address);
    EXPECT_EQ(get_buffer_label_address(port.handle + 1, &labels), video_out_invalid_handle);
    ASSERT_EQ(get_buffer_label_address(port.handle, &labels), 16);
    ASSERT_NE(fake.port.load(), nullptr);
    EXPECT_EQ(labels, reinterpret_cast<std::uintptr_t>(fake.port.load()->buffer_labels.data()));
    EXPECT_EQ(submit_eop_flip(port.handle + 1, 1, 42), video_out_invalid_handle);
    EXPECT_EQ(submit_eop_flip(port.handle, max_display_buffers, 42), video_out_invalid_index);

    EXPECT_EQ(submit_eop_flip(port.handle, 1, 42), 0);
    EXPECT_EQ(flip_status(port.handle).flip_pending_num, 0);  // not before the GPU gets there
    reinterpret_cast<std::uint64_t*>(labels)[1] = 1;           // what the packet writes first
    // A flip still pending at a vblank holds gc_queue_num up: hold the present of the first one.
    fake.hold = true;
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
    const FlipStatus queued = flip_status(port.handle);
    EXPECT_EQ(queued.flip_pending_num, 1);
    EXPECT_EQ(queued.gc_queue_num, 1);
    fake.hold = false;
    const std::uint32_t timeout_us = 1'000'000;
    const WaitOutcome w = wait_for(port.queue, &timeout_us);
    EXPECT_EQ(w.result, 0);
    EXPECT_EQ(event_flip_arg(w.event), 42);
    const FlipStatus status = flip_status(port.handle);
    EXPECT_EQ(status.count, 1u);
    EXPECT_EQ(status.current_buffer, 1);
    EXPECT_EQ(status.flip_pending_num, 0);
    EXPECT_EQ(status.gc_queue_num, 0);
    EXPECT_EQ(fake.commands, 0);
    {
        const std::scoped_lock lock{fake.mutex};
        EXPECT_EQ(fake.prepared, (std::vector<std::uint64_t>{Port::buffers[1]}));
    }
    start_video_out(nullptr);
}

// What the GNM driver hands to Liverpool, recorded. map_queue takes the lowest free of 8 slots.
struct FakeGpu final : CommandProcessor {
    struct Buffer {
        const std::uint32_t* at;
        std::size_t dwords;
        bool operator==(const Buffer&) const = default;
    };
    struct Ring {
        std::uint32_t* ring = nullptr;
        std::uint32_t dwords = 0;
        std::uint32_t* read_pointer = nullptr;
        std::uint32_t pipe = 0;
    };
    void submit_gfx(std::span<const std::uint32_t> dcb,
                    std::span<const std::uint32_t> ccb) override {
        const std::scoped_lock lock{mutex};
        draws.push_back({dcb.data(), dcb.size()});
        constants.push_back({ccb.data(), ccb.size()});
    }
    void submit_asc(std::uint32_t gnm_vqid, std::span<const std::uint32_t> acb) override {
        const std::scoped_lock lock{mutex};
        computes.emplace_back(gnm_vqid, Buffer{acb.data(), acb.size()});
    }
    void submit_done() override { ++done; }
    bool is_gpu_idle() override { return idle; }
    std::uint32_t map_queue(std::uint32_t* ring, std::uint32_t dwords, std::uint32_t* read_pointer,
                            std::uint32_t pipe) override {
        const auto free = std::ranges::find(rings, nullptr, &Ring::ring);
        *free = {ring, dwords, read_pointer, pipe};
        return static_cast<std::uint32_t>(free - rings.begin());
    }
    bool unmap_queue(std::uint32_t index) override {
        if (index >= rings.size() || rings[index].ring == nullptr) {
            return false;
        }
        rings[index] = {};
        return true;
    }
    std::span<const std::uint32_t> queue_ring(std::uint32_t index) override {
        if (index >= rings.size() || rings[index].ring == nullptr) {
            return {};
        }
        return {rings[index].ring, rings[index].dwords};
    }
    // The draw buffers since the last call.
    std::vector<Buffer> take_draws() {
        const std::scoped_lock lock{mutex};
        std::vector<Buffer> taken;
        taken.swap(draws);
        return taken;
    }

    std::mutex mutex;
    std::vector<Buffer> draws;
    std::vector<Buffer> constants;
    std::vector<std::pair<std::uint32_t, Buffer>> computes;
    std::array<Ring, 8> rings{};
    std::atomic<int> done{0};
    std::atomic<bool> idle{true};
};

// Installs `gpu` for the GNM driver for one test, and ends the frame on entry so that the next
// submit starts with the init sequence.
struct UseGpu {
    explicit UseGpu(FakeGpu& gpu) {
        start_gnm(&gpu);
        EXPECT_EQ(sceGnmSubmitDone(), 0);
        gpu.done = 0;
    }
    ~UseGpu() { start_gnm(nullptr); }
};

// A page of guest memory.
struct GuestPage {
    GuestPage() {
        bb::hle::kernel::vmem::Mapping m;
        m.length = bb::hle::kernel::vmem::page_size;
        m.prot = PROT_READ | PROT_WRITE;
        m.host_flags = MAP_PRIVATE | MAP_ANONYMOUS;
        EXPECT_EQ(bb::hle::kernel::vmem::map(m, base), 0);
    }
    ~GuestPage() { EXPECT_EQ(bb::hle::kernel::vmem::unmap(base, bb::hle::kernel::vmem::page_size), 0); }
    std::uint32_t* dwords() const { return reinterpret_cast<std::uint32_t*>(base); }
    std::uintptr_t base = 0;
};

// Upstream's init sequence for SDK 2.x starts with a CLEAR_STATE and is 0x78 dwords long.
bool is_init_sequence(const FakeGpu::Buffer& b) {
    return b.dwords == 0x78 && b.at[0] == 0xc0001200u;
}

constexpr std::uint32_t prepare_flip_header = 0xc03e1000u;  // NOP of 64 dwords
constexpr std::uint32_t write_data_header = 0xc0033700u;    // WRITE_DATA, 5 dwords
constexpr std::uint32_t patched_flip = 0x68750776u;

// Submits buffers to the GPU, the constant buffers beside them; the first submit of a frame sends
// the init sequence before them, and validation submits nothing.
TEST(HleVideo, GnmSubmitsHandTheBuffersToTheGpu) {
    FakeGpu gpu;
    const UseGpu use(gpu);
    const GuestPage page;
    const std::array<std::uint64_t, 2> draw{page.base, page.base + 0x100};
    const std::array<std::uint32_t, 2> draw_sizes{12, 0x40};
    const std::array<std::uint64_t, 2> ccb{page.base + 0x200, 0};
    const std::array<std::uint32_t, 2> ccb_sizes{40, 0};
    EXPECT_EQ(gnm_submit_other(2, draw.data(), draw_sizes.data(), nullptr, nullptr),
              validation_not_enabled);
    EXPECT_TRUE(gpu.take_draws().empty());

    EXPECT_EQ(gnm_submit(2, draw.data(), draw_sizes.data(), ccb.data(), ccb_sizes.data()), 0);
    EXPECT_EQ(gnm_submit(1, draw.data(), draw_sizes.data(), nullptr, nullptr), 0);
    const std::vector<FakeGpu::Buffer> draws = gpu.take_draws();
    ASSERT_EQ(draws.size(), 4u);
    EXPECT_TRUE(is_init_sequence(draws[0]));
    EXPECT_EQ(draws[1], (FakeGpu::Buffer{page.dwords(), 3}));
    EXPECT_EQ(draws[2], (FakeGpu::Buffer{page.dwords() + 0x40, 0x10}));
    EXPECT_EQ(draws[3], (FakeGpu::Buffer{page.dwords(), 3}));  // no init sequence: same frame
    {
        const std::scoped_lock lock{gpu.mutex};
        ASSERT_EQ(gpu.constants.size(), 4u);
        EXPECT_EQ(gpu.constants[0].dwords, 0u);
        EXPECT_EQ(gpu.constants[1], (FakeGpu::Buffer{page.dwords() + 0x80, 10}));
        EXPECT_EQ(gpu.constants[2].dwords, 0u);  // a null CCB
        EXPECT_EQ(gpu.constants[3].dwords, 0u);
    }

    // A new frame starts with the init sequence again.
    EXPECT_EQ(sceGnmSubmitDone(), 0);
    EXPECT_EQ(gpu.done, 1);
    EXPECT_EQ(gnm_submit(1, draw.data(), draw_sizes.data(), nullptr, nullptr), 0);
    const std::vector<FakeGpu::Buffer> next = gpu.take_draws();
    ASSERT_EQ(next.size(), 2u);
    EXPECT_TRUE(is_init_sequence(next[0]));
}

// The checks of upstream's submits: nothing is submitted for a call that fails one.
TEST(HleVideo, GnmSubmitsRefuseBadArguments) {
    FakeGpu gpu;
    const UseGpu use(gpu);
    const GuestPage page;
    const std::uint64_t draw = page.base;
    const std::array<std::uint32_t, 3> sizes{0, 0x400000, 12};
    const std::uint32_t big = 0x400000;
    EXPECT_EQ(gnm_submit(1, nullptr, &sizes[2], nullptr, nullptr), submission_invalid_argument);
    EXPECT_EQ(gnm_submit(1, &draw, nullptr, nullptr, nullptr), submission_invalid_argument);
    EXPECT_EQ(gnm_submit(1, &draw, &sizes[0], nullptr, nullptr), submission_invalid_argument);
    EXPECT_EQ(gnm_submit(1, &draw, &sizes[1], nullptr, nullptr), submission_invalid_argument);
    EXPECT_EQ(gnm_submit(1, &draw, &sizes[2], nullptr, &sizes[2]), submission_invalid_argument);
    EXPECT_EQ(gnm_submit(1, &draw, &sizes[2], &draw, &big), submission_invalid_argument);
    EXPECT_EQ(gnm_submit_and_flip(0, &draw, &sizes[2], nullptr, nullptr, main_port_handle, 0, 1, 0),
              submission_invalid_argument);
    EXPECT_EQ(gnm_submit_and_flip(1, &draw, &sizes[0], nullptr, nullptr, main_port_handle, 0, 1, 0),
              submission_invalid_argument);
    std::array<std::uint32_t, 0x40> small{};
    EXPECT_EQ(gnm_request_flip_and_submit_done(small.data(), 0xfc, main_port_handle, 0, 1, 0),
              submission_not_enough_resources);
    EXPECT_EQ(small[0], 0u);
    EXPECT_TRUE(gpu.take_draws().empty());
    EXPECT_EQ(gpu.done, 0);
}

// Submit-and-flip turns the flip packet at the end of the last buffer into a write of the
// buffer's flip label and a PatchedFlip packet. When the GPU reaches it (the GfxFlip interrupt),
// the flip is prepared on the GPU's thread, not sent as a command, and completes with its event.
TEST(HleVideo, SubmitAndFlipPatchesTheFlipPacketAndFlipsAtItsInterrupt) {
    FakeGpu gpu;
    const UseGpu use(gpu);
    static FakePresenter fake;
    start_video_out(&fake);
    const Port port;
    const GuestPage page;
    std::uint32_t* cmd = page.dwords() + 0x100;  // 4 dwords of work, then the flip packet
    cmd[4] = prepare_flip_header;
    cmd[5] = 0x68750777u;  // PrepareFlip
    const std::uint64_t draw = page.base + 0x400;
    const std::uint32_t bytes = (4 + 64) * 4;
    EXPECT_EQ(gnm_submit_and_flip(1, &draw, &bytes, nullptr, nullptr, port.handle, 1, 1, 42), 0);
    std::uintptr_t labels = 0;
    ASSERT_EQ(get_buffer_label_address(port.handle, &labels), 16);
    const std::uint64_t label = labels + 8;
    EXPECT_EQ(cmd[4], write_data_header);
    EXPECT_EQ(cmd[5], 0x500u);
    EXPECT_EQ(cmd[6] | std::uint64_t{cmd[7]} << 32, label);
    EXPECT_EQ(cmd[8], 1u);
    EXPECT_EQ(cmd[9], 0xc0391000u);  // NOP over the rest of the packet
    EXPECT_EQ(cmd[10], patched_flip);
    const std::vector<FakeGpu::Buffer> draws = gpu.take_draws();
    ASSERT_EQ(draws.size(), 2u);
    EXPECT_TRUE(is_init_sequence(draws[0]));
    EXPECT_EQ(draws[1], (FakeGpu::Buffer{cmd, 68}));
    EXPECT_EQ(flip_status(port.handle).flip_pending_num, 0);  // not before the GPU gets there

    // What Liverpool does with the two packets.
    *reinterpret_cast<std::uint64_t*>(label) = 1;
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
    const std::uint32_t timeout_us = 1'000'000;
    const WaitOutcome w = wait_for(port.queue, &timeout_us);
    EXPECT_EQ(w.result, 0);
    EXPECT_EQ(event_flip_arg(w.event), 42);
    const FlipStatus status = flip_status(port.handle);
    EXPECT_EQ(status.count, 1u);
    EXPECT_EQ(status.current_buffer, 1);
    EXPECT_EQ(status.flip_pending_num, 0);
    EXPECT_EQ(status.gc_queue_num, 0);
    EXPECT_EQ(fake.commands, 0);
    {
        const std::scoped_lock lock{fake.mutex};
        EXPECT_EQ(fake.prepared, (std::vector<std::uint64_t>{Port::buffers[1]}));
    }
    start_video_out(nullptr);
}

// The label variant of the flip packet also writes the game's label after the flip.
TEST(HleVideo, SubmitAndFlipKeepsTheLabelOfTheFlipPacket) {
    FakeGpu gpu;
    const UseGpu use(gpu);
    const Port port;
    const GuestPage page;
    std::uint32_t* cmd = page.dwords();
    const std::uint64_t game_label = page.base + 0x800;
    cmd[0] = prepare_flip_header;
    cmd[1] = 0x68750778u;  // PrepareFlipLabel
    cmd[2] = static_cast<std::uint32_t>(game_label);
    cmd[3] = static_cast<std::uint32_t>(game_label >> 32);
    cmd[4] = 0x1234;
    const std::uint64_t draw = page.base;
    const std::uint32_t bytes = 64 * 4;
    EXPECT_EQ(gnm_submit_and_flip(1, &draw, &bytes, nullptr, nullptr, port.handle, 0, 1, 3), 0);
    EXPECT_EQ(cmd[0], write_data_header);
    EXPECT_EQ(cmd[5], 0xc0341000u);
    EXPECT_EQ(cmd[6], patched_flip);
    EXPECT_EQ(cmd[0x3b], write_data_header);
    EXPECT_EQ(cmd[0x3c], 0x500u);
    EXPECT_EQ(cmd[0x3d], static_cast<std::uint32_t>(game_label));
    EXPECT_EQ(cmd[0x3e], static_cast<std::uint32_t>(game_label >> 32));
    EXPECT_EQ(cmd[0x3f], 0x1234u);
    std::uintptr_t labels = 0;
    ASSERT_EQ(get_buffer_label_address(port.handle, &labels), 16);
    *reinterpret_cast<std::uint64_t*>(labels) = 1;
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
    EXPECT_EQ(event_flip_arg(wait_for(port.queue, nullptr).event), 3);
}

// RequestFlipAndSubmitDone writes the flip packet itself, submits it and ends the frame.
TEST(HleVideo, RequestFlipAndSubmitDoneSubmitsAFlipPacket) {
    FakeGpu gpu;
    const UseGpu use(gpu);
    const Port port;
    const GuestPage page;
    std::uint32_t* cmd = page.dwords();
    EXPECT_EQ(gnm_request_flip_and_submit_done(cmd, 0x100, port.handle, 0, 1, 8), 0);
    EXPECT_EQ(cmd[0], write_data_header);
    EXPECT_EQ(cmd[5], 0xc0391000u);
    EXPECT_EQ(cmd[6], patched_flip);
    const std::vector<FakeGpu::Buffer> draws = gpu.take_draws();
    ASSERT_EQ(draws.size(), 1u);  // without the init sequence, as upstream
    EXPECT_EQ(draws.back(), (FakeGpu::Buffer{cmd, 0x40}));
    EXPECT_EQ(gpu.done, 1);
    std::uintptr_t labels = 0;
    ASSERT_EQ(get_buffer_label_address(port.handle, &labels), 16);
    *reinterpret_cast<std::uint64_t*>(labels) = 1;
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
    EXPECT_EQ(event_flip_arg(wait_for(port.queue, nullptr).event), 8);
}

// SubmitDone while the GPU is busy takes the submission lock: the next submit waits until the GPU
// reports that it is idle.
TEST(HleVideo, SubmitDoneBlocksSubmitsUntilTheGpuIsIdle) {
    FakeGpu gpu;
    const UseGpu use(gpu);
    const GuestPage page;
    const std::uint64_t draw = page.base;
    const std::uint32_t bytes = 12;
    EXPECT_EQ(sceGnmAreSubmitsAllowed(), 1);
    gpu.idle = false;
    EXPECT_EQ(sceGnmSubmitDone(), 0);
    EXPECT_EQ(gpu.done, 1);
    EXPECT_EQ(sceGnmAreSubmitsAllowed(), 0);
    std::atomic<bool> submitted{false};
    std::thread submitter([&] {
        EXPECT_EQ(gnm_submit(1, &draw, &bytes, nullptr, nullptr), 0);
        submitted = true;
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(submitted);
    EXPECT_TRUE(gpu.take_draws().empty());
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    submitter.join();
    EXPECT_EQ(sceGnmAreSubmitsAllowed(), 1);
    EXPECT_EQ(gpu.take_draws().size(), 2u);  // the init sequence and the buffer

    gpu.idle = true;
    EXPECT_EQ(sceGnmSubmitDone(), 0);
    EXPECT_EQ(sceGnmAreSubmitsAllowed(), 1);
}

// A map checks its arguments as shadPS4 does and hands out positive ids (the game treats 0 as a
// failed map, 0x108c7b0); a doorbell submits the ring from the previous offset on, in two parts
// when the writer wrapped.
TEST(HleVideo, ComputeQueuesSubmitTheirRing) {
    const GuestPage page;
    auto* ring = page.dwords();
    auto* read_pointer = ring + 0x100;
    *read_pointer = 7;
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring, 0x40, read_pointer), compute_queue_internal);
    FakeGpu gpu;
    const UseGpu use(gpu);
    EXPECT_EQ(gnm_map_compute_queue(7, 0, ring, 0x40, read_pointer), compute_queue_invalid_pipe);
    EXPECT_EQ(gnm_map_compute_queue(0, 8, ring, 0x40, read_pointer), compute_queue_invalid_queue);
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring, 0x30, read_pointer), compute_queue_invalid_size);
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring + 1, 0x40, read_pointer),
              compute_queue_invalid_ring);
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring + 1, 0x30, read_pointer),
              compute_queue_invalid_ring);  // alignment is checked before the size
    EXPECT_EQ(gnm_map_compute_queue(0, 0, ring, 0x40, reinterpret_cast<std::uint32_t*>(0x10)),
              compute_queue_invalid_read_pointer);
    EXPECT_EQ(*read_pointer, 7u);
    const std::int32_t first = gnm_map_compute_queue(0, 0, ring, 0x40, read_pointer);
    const std::int32_t second = gnm_map_compute_queue_with_priority(3, 1, ring, 0x40, read_pointer,
                                                                    2);
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 2);
    EXPECT_EQ(*read_pointer, 0u);
    EXPECT_EQ(gpu.rings[1].pipe, 3u);
    EXPECT_EQ(gpu.rings[1].read_pointer, read_pointer);

    gnm_ding_dong(first, 4);
    gnm_ding_dong(first, 4);     // nothing new
    gnm_ding_dong(first, 0x3c);
    gnm_ding_dong(first, 8);     // wrapped
    gnm_ding_dong(first, 0);     // up to the end
    gnm_ding_dong(first, 0x41);  // past the ring: logged and ignored
    gnm_ding_dong(0, 4);         // logged and ignored
    gnm_ding_dong(5, 4);         // not mapped
    using Submit = std::pair<std::uint32_t, FakeGpu::Buffer>;
    EXPECT_EQ(gpu.computes, (std::vector<Submit>{
                                {1, {ring, 4}},
                                {1, {ring + 4, 0x38}},
                                {1, {ring + 0x3c, 4}},
                                {1, {ring, 8}},
                                {1, {ring + 8, 0x38}},
                            }));
    EXPECT_EQ(gnm_unmap_compute_queue(first), 0);
    EXPECT_EQ(gpu.rings[0].ring, nullptr);
    EXPECT_EQ(gnm_unmap_compute_queue(first), 0);  // unknown: logged
    EXPECT_EQ(gnm_unmap_compute_queue(0), 0);
    EXPECT_EQ(gnm_unmap_compute_queue(second), 0);
    EXPECT_EQ(gpu.rings[1].ring, nullptr);
}

// The end-of-pipe and compute interrupts wake the queues registered for them; other interrupts
// trigger nothing.
TEST(HleVideo, GpuInterruptsTriggerGnmEvents) {
    auto* const irq = Platform::IrqC::Instance();
    std::uint64_t queue = 0;
    ASSERT_EQ(bb::hle::kernel::equeue_create(&queue, "GnmEop"), 0);
    EXPECT_LT(sceGnmAddEqEvent(queue + 1000, gfx_eop_event, 0), 0);
    EXPECT_LT(sceGnmAddEqEvent(queue, 0x41, 0), 0);
    EXPECT_EQ(sceGnmAddEqEvent(queue, gfx_eop_event, 0x98), 0);
    EXPECT_EQ(sceGnmAddEqEvent(queue, gfx_eop_event, 0x99), 0);  // replaces the first
    EXPECT_EQ(sceGnmAddEqEvent(queue, 3, 0x33), 0);
    EXPECT_EQ(sceGnmAddEqEvent(queue, 9, 0x9), 0);  // GpuIdle: no GPU event

    irq->Signal(Platform::InterruptId::GfxEop);
    const std::uint32_t timeout = 1'000'000;
    WaitOutcome w = wait_for(queue, &timeout);
    EXPECT_EQ(w.result, 0);
    EXPECT_EQ(w.count, 1);
    EXPECT_EQ(w.event.id, gfx_eop_event);
    EXPECT_EQ(w.event.kind, gnm_event_kind);
    EXPECT_EQ(w.event.user_data, 0x99u);
    EXPECT_EQ(sceGnmGetEqEventType(&w.event), 0x40);
    irq->Signal(Platform::InterruptId::Compute3RelMem);
    w = wait_for(queue, &timeout);
    EXPECT_EQ(w.event.id, 3u);
    EXPECT_EQ(w.event.user_data, 0x33u);
    EXPECT_EQ(sceGnmGetEqEventType(&w.event), 3);

    const std::uint32_t no_wait = 0;
    const auto timed_out = static_cast<std::int32_t>(0x8002003c);  // ETIMEDOUT
    irq->Signal(Platform::InterruptId::GpuIdle);
    EXPECT_EQ(wait_for(queue, &no_wait).result, timed_out);
    // Without the registration the interrupt triggers nothing.
    EXPECT_LT(sceGnmDeleteEqEvent(queue + 1000, gfx_eop_event), 0);
    EXPECT_EQ(sceGnmDeleteEqEvent(queue, gfx_eop_event), 0);
    EXPECT_EQ(sceGnmDeleteEqEvent(queue, 3), 0);
    EXPECT_EQ(sceGnmDeleteEqEvent(queue, 9), 0);
    irq->Signal(Platform::InterruptId::GfxEop);
    irq->Signal(Platform::InterruptId::Compute3RelMem);
    EXPECT_EQ(wait_for(queue, &no_wait).result, timed_out);
    EXPECT_EQ(bb::hle::kernel::equeue_delete(queue), 0);
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
    const std::array<Expected, 32> expected{{
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
        {"libSceGnmDriver", "gObODli-OH8",
         reinterpret_cast<void*>(&gnm_request_flip_and_submit_done)},
        {"libSceGnmDriver", "A+uGq+3KFtQ",
         reinterpret_cast<void*>(&gnm_map_compute_queue_with_priority)},
        {"libSceGnmDriver", "b08AgtPlHPg", reinterpret_cast<void*>(&sceGnmAreSubmitsAllowed)},
        {"libSceGnmDriver", "gAhCn6UiU4Y", reinterpret_cast<void*>(&sceGnmSetVsShader)},
        {"libSceGnmDriver", "V31V01UiScY", reinterpret_cast<void*>(&sceGnmSetVsShader)},
        {"libSceGnmDriver", "bQVd5YzCal0", reinterpret_cast<void*>(&sceGnmSetPsShader)},
        {"libSceGnmDriver", "4MgRw-bVNQU", reinterpret_cast<void*>(&sceGnmSetPsShader)},
        {"libSceGnmDriver", "KXltnCwEJHQ", reinterpret_cast<void*>(&sceGnmSetCsShader)},
        {"libSceGnmDriver", "FUHG8sQ3R58", reinterpret_cast<void*>(&sceGnmSetEsShader)},
        {"libSceGnmDriver", "UJwNuMBcUAk", reinterpret_cast<void*>(&sceGnmSetGsShader)},
        {"libSceGnmDriver", "nLM2i2+65hA", reinterpret_cast<void*>(&sceGnmSetGsShader)},
        {"libSceGnmDriver", "VJNjFtqiF5w", reinterpret_cast<void*>(&sceGnmSetHsShader)},
        {"libSceGnmDriver", "GNlx+y7xPdE", reinterpret_cast<void*>(&sceGnmSetHsShader)},
        {"libSceGnmDriver", "vckdzbQ46SI", reinterpret_cast<void*>(&sceGnmSetLsShader)},
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

// #471: the ring address is fixed, inside the guest window, and held by a reservation that no
// guest mapping can take.
TEST(GnmDriver, TessellationFactorRingIsReserved) {
    const std::uint64_t address = sceGnmGetTheTessellationFactorRingBufferBaseAddress();
    EXPECT_EQ(address, tessellation_factor_ring_address);
    EXPECT_EQ(sceGnmGetTheTessellationFactorRingBufferBaseAddress(), address);
    EXPECT_EQ(address + tessellation_factor_ring_size, bb::hle::kernel::vmem::window_end);
    const auto region = bb::hle::kernel::vmem::find(address);
    ASSERT_TRUE(region.has_value());
    EXPECT_TRUE(region->reserved);
    EXPECT_LE(region->start, address);
    EXPECT_GE(region->end, address + tessellation_factor_ring_size);
}

}  // namespace
