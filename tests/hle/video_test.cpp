#include "hle/video/video.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/equeue.hpp"
#include "hle/video/gnm_shader.hpp"
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

std::uint64_t flip_count(std::int32_t port) {
    std::array<unsigned char, 0x28> status;
    status.fill(0xa5);
    EXPECT_EQ(sceVideoOutGetFlipStatus(port, status.data()), 0);
    for (std::size_t i = 0; i < status.size(); ++i) {
        if (i < flip_count_offset || i >= flip_count_offset + 8) {
            EXPECT_EQ(status[i], 0xa5) << i;  // only the count is written
        }
    }
    std::uint64_t count = 0;
    std::memcpy(&count, status.data() + flip_count_offset, sizeof count);
    return count;
}

// A port set up as at 0x11e488e..0x11e4915: open, flip queue, two 1920x1080 buffers.
struct Port {
    Port() {
        handle = sceVideoOutOpen(0, 0, 0, nullptr);
        EXPECT_GT(handle, 0);
        EXPECT_EQ(bb::hle::kernel::equeue_create(&queue, "FlipEventQueue"), 0);
        EXPECT_EQ(sceVideoOutAddFlipEvent(queue, handle, 0x1234), 0);
        std::array<unsigned char, 0x78> attr{};
        EXPECT_EQ(sceVideoOutSetBufferAttribute(attr.data(), 0x80000000, 0, 0, 1920, 1080, 1920),
                  0);
        const std::array<std::uint64_t, 2> buffers{0x10000000, 0x10800000};
        EXPECT_EQ(sceVideoOutRegisterBuffers(handle, 0, buffers.data(), 2, attr.data()), 0);
    }
    ~Port() {
        EXPECT_EQ(sceVideoOutClose(handle), 0);
        EXPECT_EQ(bb::hle::kernel::equeue_delete(queue), 0);
    }
    std::int32_t handle = 0;
    std::uint64_t queue = 0;
};

TEST(HleVideo, OpenHandsOutDistinctHandles) {
    const std::int32_t a = sceVideoOutOpen(0, 0, 0, nullptr);
    const std::int32_t b = sceVideoOutOpen(0, 0, 0, nullptr);
    EXPECT_GT(a, 0);
    EXPECT_GT(b, 0);
    EXPECT_NE(a, b);
    EXPECT_EQ(video_out_configure(a, 0), 0);
    EXPECT_EQ(sceVideoOutClose(a), 0);
    EXPECT_EQ(sceVideoOutClose(a), video_out_error);
    EXPECT_EQ(video_out_configure(a, 0), video_out_error);
    EXPECT_NE(sceVideoOutOpen(0, 0, 0, nullptr), a);  // not reused
    EXPECT_EQ(sceVideoOutClose(b), 0);
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
    const std::int32_t port = sceVideoOutOpen(0, 0, 0, nullptr);
    EXPECT_LT(sceVideoOutAddFlipEvent(queue + 1000, port, 0), 0);  // no such queue
    EXPECT_LT(sceVideoOutRegisterBuffers(port, max_buffers - 1, &buffer, 2, block.data()), 0);
    EXPECT_LT(sceVideoOutRegisterBuffers(port, 0, &buffer, -1, block.data()), 0);
    EXPECT_EQ(sceVideoOutClose(port), 0);
    EXPECT_EQ(bb::hle::kernel::equeue_delete(queue), 0);
}

// 0x26d65d6 registers with count 0 when the configured buffer count is 0 and fails on a
// negative result, so count 0 succeeds on an open port.
TEST(HleVideo, RegisteringNoBuffersSucceeds) {
    std::array<unsigned char, 0x40> block{};
    const std::uint64_t buffer = 0x10000000;
    const std::int32_t port = sceVideoOutOpen(0, 0, 0, nullptr);
    EXPECT_EQ(sceVideoOutRegisterBuffers(port, 0, &buffer, 0, block.data()), 0);
    EXPECT_EQ(sceVideoOutClose(port), 0);
    EXPECT_LT(sceVideoOutRegisterBuffers(port, 0, &buffer, 0, block.data()), 0);  // closed
}

TEST(HleVideo, BufferAttributeFillsOnlyOurLayout) {
    std::array<unsigned char, 0x40> block;
    block.fill(0xa5);
    EXPECT_EQ(sceVideoOutSetBufferAttribute(block.data() + 1, 0x80000000, 1, 0, 1920, 1080, 1920),
              0);
    BufferAttribute a;
    std::memcpy(&a, block.data() + 1, sizeof a);
    EXPECT_EQ(a.format, 0x80000000u);
    EXPECT_EQ(a.tiling, 1u);
    EXPECT_EQ(a.width, 1920u);
    EXPECT_EQ(a.height, 1080u);
    EXPECT_EQ(a.pitch, 1920u);
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
    EXPECT_EQ(outcome.event, (GuestEvent{static_cast<std::uint64_t>(port.handle),
                                         flip_event_kind, 0, 1, 77, 0x1234}));
    EXPECT_EQ(flip_count(port.handle), 1u);
    EXPECT_LE(elapsed, Tick(1) + 200ms);  // the next tick, with room for a loaded machine
}

// The observer sees each flip with its buffer, before the flip's event goes out.
std::atomic<int> observed_flips{0};
std::atomic<std::uint64_t> observed_address{0};
std::atomic<std::uint32_t> observed_width{0};
void observe_flip(std::int32_t, std::int32_t index, std::uint64_t address,
                  const BufferAttribute& attribute) {
    observed_address = index == 1 ? address : 0;
    observed_width = attribute.width;
    ++observed_flips;
}

TEST(HleVideo, FlipObserverSeesTheBufferBeforeTheEvent) {
    const Port port;
    set_flip_observer(&observe_flip);
    EXPECT_EQ(sceVideoOutSubmitFlip(port.handle, 1, 1, 5), 0);
    const std::uint32_t timeout_us = 1'000'000;
    const WaitOutcome outcome = wait_for(port.queue, &timeout_us);
    set_flip_observer(nullptr);
    EXPECT_EQ(outcome.result, 0);
    EXPECT_EQ(observed_flips, 1);
    EXPECT_EQ(observed_address, 0x10800000u);
    EXPECT_EQ(observed_width, 1920u);
}

TEST(HleVideo, FlipsCompleteOnePerTick) {
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
    // Three flips need three ticks after the submission: at least two full intervals.
    EXPECT_GE(std::chrono::steady_clock::now() - start, 2 * Tick(1));
}

TEST(HleVideo, FlipReachesOnlyItsPortsQueues) {
    const Port first;
    const Port second;
    EXPECT_EQ(sceVideoOutSubmitFlip(first.handle, 0, 1, 0), 0);
    const WaitOutcome w = wait_for(first.queue, nullptr);
    EXPECT_EQ(w.event.id, static_cast<std::uint64_t>(first.handle));
    const std::uint32_t timeout = 50'000;
    EXPECT_LT(wait_for(second.queue, &timeout).result, 0);  // timed out
    EXPECT_EQ(flip_count(second.handle), 0u);
}

TEST(HleVideo, GnmSubmitsSucceedAndSubmitAndFlipFlips) {
    const std::array<std::uint64_t, 2> buffers{0x20000000, 0x20010000};
    const std::array<std::uint32_t, 2> sizes{0x100, 0x40};
    EXPECT_EQ(gnm_submit(2, buffers.data(), sizes.data(), nullptr, nullptr), 0);
    EXPECT_EQ(gnm_submit_other(1, buffers.data(), sizes.data(), nullptr, nullptr), 0);

    const Port port;
    EXPECT_EQ(gnm_submit_and_flip(1, buffers.data(), sizes.data(), nullptr, nullptr, port.handle,
                                  1, 1, 5),
              0);
    EXPECT_EQ(sceGnmSubmitDone(), 0);
    const WaitOutcome w = wait_for(port.queue, nullptr);
    EXPECT_EQ(w.event.detail, 1u);
    EXPECT_EQ(w.event.data, 5);
    EXPECT_EQ(flip_count(port.handle), 1u);
}

// Each map hands out a new, positive id: the game treats 0 as a failed map (0x108c7b0).
TEST(HleVideo, MappingComputeQueuesHandsOutPositiveIds) {
    std::array<std::uint32_t, 0x400> ring{};
    std::uint32_t read_pointer = 7;
    const std::int32_t first = gnm_map_compute_queue(0, 0, ring.data(), 0x400, &read_pointer);
    const std::int32_t second = gnm_map_compute_queue(1, 0, ring.data(), 0x400, &read_pointer);
    EXPECT_GT(first, 0);
    EXPECT_GT(second, first);
    EXPECT_EQ(read_pointer, 7u);  // the game's wrapper clears it
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
    const std::array<Expected, 18> expected{{
        {"libSceVideoOut", "Up36PTk687E", reinterpret_cast<void*>(&sceVideoOutOpen)},
        {"libSceGnmDriver", "29oKvKXzEZo", reinterpret_cast<void*>(&gnm_map_compute_queue)},
        {"libSceVideoOut", "uquVH4-Du78", reinterpret_cast<void*>(&sceVideoOutClose)},
        {"libSceVideoOut", "CBiu4mCE1DA", reinterpret_cast<void*>(&video_out_configure)},
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
