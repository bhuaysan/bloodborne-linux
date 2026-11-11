// video_core's Liverpool behind the GNM driver's CommandProcessor, without a window: command
// buffers in guest memory run on Liverpool's GPU thread, an end-of-pipe packet writes its label
// and signals its interrupt, and a compute ring runs as far as it is submitted. Without the
// Presenter there is no rasterizer, so this needs no Vulkan device.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "core/memory.h"
#include "core/platform.h"
#include "hle/kernel/vmem.hpp"
#include "hle/video/video_core_presenter.hpp"

namespace {

using namespace bb::hle::video;
using namespace std::chrono_literals;

std::uint32_t type3(std::uint32_t opcode, std::uint32_t dwords) {
    return 3u << 30 | (dwords - 2) << 16 | opcode << 8;
}

bool eventually(auto&& done) {
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A page of flexible memory, which video_core's guest memory backs.
std::uint32_t* guest_page() {
    Core::Memory::Instance();  // guest memory follows the kernel's mappings from here on
    void* page = nullptr;
    EXPECT_EQ(bb::hle::kernel::map_anonymous(&page, bb::hle::kernel::vmem::page_size, 3, 0), 0);
    return static_cast<std::uint32_t*>(page);
}

// One Liverpool for the tests: its GPU thread may still run after a test.
VideoCorePresenter& liverpool() {
    static VideoCorePresenter gpu(nullptr);
    return gpu;
}

TEST(GnmLiverpool, RunsCommandBuffersWithoutAWindow) {
    CommandProcessor& gpu = liverpool();
    std::uint32_t* dwords = guest_page();
    const auto base = reinterpret_cast<std::uint64_t>(dwords);
    std::atomic<int> eops{0};
    int uid = 0;
    Platform::IrqC::Instance()->Register(
        Platform::InterruptId::GfxEop, [&](Platform::InterruptId) { ++eops; }, &uid);

    // EVENT_WRITE_EOP: write 0x77 to `label`, then interrupt.
    const std::uint64_t label = base + 0x800;
    const std::array<std::uint32_t, 6> eop{type3(0x47, 6),
                                           0x504,
                                           static_cast<std::uint32_t>(label),
                                           1u << 29 | 2u << 24 |
                                               static_cast<std::uint32_t>(label >> 32),
                                           0x77,
                                           0};
    std::ranges::copy(eop, dwords);
    gpu.submit_gfx(std::span<const std::uint32_t>(dwords, eop.size()), {});
    gpu.submit_done();
    EXPECT_TRUE(eventually([&] { return eops == 1; }));
    EXPECT_EQ(std::atomic_ref(dwords[0x200]).load(), 0x77u);
    EXPECT_TRUE(eventually([&] { return gpu.is_gpu_idle(); }));
    Platform::IrqC::Instance()->Unregister(Platform::InterruptId::GfxEop, &uid);

    // A compute ring of 4-dword NOPs: Liverpool moves the read pointer past what was submitted.
    std::uint32_t* ring = dwords + 0x100;
    std::uint32_t* read_pointer = dwords + 0x300;
    for (int i = 0; i < 0x40; i += 4) {
        ring[i] = type3(0x10, 4);
    }
    const std::uint32_t index = gpu.map_queue(ring, 0x40, read_pointer, 0);
    EXPECT_EQ(gpu.queue_ring(index).data(), ring);
    EXPECT_EQ(gpu.queue_ring(index).size(), 0x40u);
    EXPECT_TRUE(gpu.queue_ring(index + 1).empty());
    gpu.submit_asc(index + 1, gpu.queue_ring(index).first(8));
    EXPECT_TRUE(eventually([&] { return std::atomic_ref(*read_pointer).load() == 8; }));
    EXPECT_TRUE(gpu.unmap_queue(index));
    EXPECT_FALSE(gpu.unmap_queue(index));
    EXPECT_TRUE(gpu.queue_ring(index).empty());
    EXPECT_EQ(bb::hle::kernel::sceKernelMunmap(dwords, bb::hle::kernel::vmem::page_size), 0);
}

// Without the Presenter there are no frames, and presenting one does nothing.
TEST(GnmLiverpool, HasNoFramesWithoutAWindow) {
    FlipPresenter& presenter = liverpool();
    EXPECT_EQ(presenter.prepare_blank_frame(true), nullptr);
    EXPECT_EQ(presenter.prepare_last_frame(), nullptr);
    EXPECT_FALSE(presenter.must_keep_drawing());
    presenter.present(nullptr, false, true);
    EXPECT_EQ(liverpool().game_frames(), 0u);
}

}  // namespace
