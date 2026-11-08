#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/gpu_thread.hpp"
#include "gpu/pm4/parser.hpp"

namespace {

using bb::pm4::Engine;
using bb::pm4::gfx_eop_interrupt;
using bb::pm4::GpuThread;
using bb::pm4::Parser;
using bb::pm4::Queue;
using bb::pm4::RegisterMirror;
using bb::pm4::RunResult;
using bb::pm4::Stream;
using bb::pm4::SyncPoint;
using bb::pm4::type3_header;
using namespace std::chrono_literals;

void packet(std::vector<std::uint32_t>& out, std::uint8_t opcode,
            std::initializer_list<std::uint32_t> body) {
    out.push_back(type3_header(opcode, static_cast<std::uint32_t>(body.size())));
    out.insert(out.end(), body);
}

std::uint32_t lo(const void* address) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uint64_t>(address));
}
std::uint32_t hi(const void* address) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uint64_t>(address) >> 32);
}

// A parser that records its sync points and interrupts.
struct Fixture {
    RegisterMirror regs;
    Parser parser{regs};
    std::vector<SyncPoint> syncs;
    std::vector<std::uint32_t> interrupts;

    Fixture() {
        parser.set_sync([this](SyncPoint point) { syncs.push_back(point); });
        parser.set_interrupt([this](std::uint32_t source) { interrupts.push_back(source); });
    }

    RunResult run(const std::vector<std::uint32_t>& buffer, Engine engine = Engine::draw) {
        Stream stream(buffer, engine);
        return parser.run(stream);
    }
};

TEST(MemoryPackets, WriteDataWritesMemoryAndRegisters) {
    Fixture f;
    std::array<std::uint32_t, 4> mem{};
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_WRITE_DATA, {0x04000500, lo(&mem[0]), hi(&mem[0]), 1, 2});  // the game's
    packet(dcb, IT_WRITE_DATA, {0x00010200, lo(&mem[3]), hi(&mem[3]), 7, 8});  // one address
    packet(dcb, IT_WRITE_DATA, {0, 0xA300, 0, 5, 6});                           // registers
    packet(dcb, IT_WRITE_DATA, {0x80000500, lo(&mem[2]), hi(&mem[2]), 9});     // CE's: skipped
    EXPECT_EQ(f.run(dcb), RunResult::done);
    EXPECT_EQ(mem, (std::array<std::uint32_t, 4>{1, 2, 0, 8}));
    EXPECT_EQ(f.regs.get(0xA300), 5u);
    EXPECT_EQ(f.regs.get(0xA301), 6u);
    EXPECT_EQ(f.syncs, (std::vector{SyncPoint::before_write, SyncPoint::before_write}));

    // The CE runs the WRITE_DATA with engine select 2 (0x11fd520) and skips the others.
    std::vector<std::uint32_t> ccb;
    packet(ccb, IT_WRITE_DATA, {0x80000500, lo(&mem[2]), hi(&mem[2]), 9});
    packet(ccb, IT_WRITE_DATA, {0x00000500, lo(&mem[0]), hi(&mem[0]), 9});
    EXPECT_EQ(f.run(ccb, Engine::constant), RunResult::done);
    EXPECT_EQ(mem, (std::array<std::uint32_t, 4>{1, 2, 9, 8}));
}

TEST(MemoryPackets, CopyDataMovesBetweenMemoryAndRegisters) {
    Fixture f;
    std::array<std::uint32_t, 4> mem{0x11, 0x22, 0, 0};
    std::vector<std::uint32_t> dcb;
    // The game's copy into VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE (0x107be30).
    packet(dcb, IT_COPY_DATA, {0x1, lo(&mem[0]), hi(&mem[0]), 0xA2CB, 0});
    packet(dcb, IT_COPY_DATA, {0x10505, 0x33, 0x44, lo(&mem[2]), hi(&mem[2])});  // immediate, 64
    packet(dcb, IT_COPY_DATA, {0x0, 0xA2CB, 0, 0xA2CC, 0});                       // register
    EXPECT_EQ(f.run(dcb), RunResult::done);
    EXPECT_EQ(f.regs.get(0xA2CB), 0x11u);
    EXPECT_EQ(f.regs.get(0xA2CC), 0x11u);
    EXPECT_EQ(mem, (std::array<std::uint32_t, 4>{0x11, 0x22, 0x33, 0x44}));
    EXPECT_EQ(f.syncs, std::vector{SyncPoint::before_write});
}

TEST(MemoryPackets, DmaDataCopiesAndFills) {
    Fixture f;
    std::array<std::uint8_t, 8> src{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<std::uint8_t, 8> copy{};
    std::array<std::uint32_t, 3> fill{};
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_DMA_DATA__CI__VI, {0, lo(&src[1]), hi(&src[1]), lo(&copy[2]), hi(&copy[2]), 5});
    packet(dcb, IT_DMA_DATA__CI__VI, {0x40000000, 0xAB, 0, lo(&fill[0]), hi(&fill[0]), 8});
    packet(dcb, IT_DMA_DATA__CI__VI, {0x40100000, 0xCD, 0, lo(&fill[2]), hi(&fill[2]), 4});  // GDS
    EXPECT_EQ(f.run(dcb), RunResult::done);
    EXPECT_EQ(copy, (std::array<std::uint8_t, 8>{0, 0, 2, 3, 4, 5, 6, 0}));
    EXPECT_EQ(fill, (std::array<std::uint32_t, 3>{0xAB, 0xAB, 0}));
    EXPECT_EQ(f.syncs.size(), 2u);
}

TEST(MemoryPackets, LabelWritesAndInterrupts) {
    Fixture f;
    alignas(8) std::array<std::uint32_t, 8> mem{};
    std::vector<std::uint32_t> dcb;
    // EVENT_WRITE_EOP: 32-bit data, no interrupt; 64-bit data with interrupt (INT_SEL 2).
    packet(dcb, IT_EVENT_WRITE_EOP, {0x504, lo(&mem[0]), 1u << 29 | hi(&mem[0]), 0xA, 0xF});
    packet(dcb, IT_EVENT_WRITE_EOP,
           {0x504, lo(&mem[2]), 2u << 29 | 2u << 24 | hi(&mem[2]), 0xB, 0xC});
    packet(dcb, IT_EVENT_WRITE_EOP, {0x504, 0, 1u << 24, 0, 0});  // interrupt only
    // EVENT_WRITE_EOS: command 2 writes the data; command 1 (GDS) is skipped.
    packet(dcb, IT_EVENT_WRITE_EOS, {0x614, lo(&mem[4]), 2u << 29 | hi(&mem[4]), 0xD});
    packet(dcb, IT_EVENT_WRITE_EOS, {0x614, lo(&mem[5]), 1u << 29 | hi(&mem[5]), 0x10001});
    // RELEASE_MEM as the game's compute writer: INT_SEL 3, 32-bit data. Then the GPU clock.
    packet(dcb, IT_RELEASE_MEM__CI__VI,
           {0x514, 1u << 29 | 3u << 24, lo(&mem[5]), hi(&mem[5]), 0xE, 0});
    packet(dcb, IT_RELEASE_MEM__CI__VI, {0x514, 3u << 29, lo(&mem[6]), hi(&mem[6]), 0, 0});
    EXPECT_EQ(f.run(dcb), RunResult::done);
    EXPECT_EQ(mem[0], 0xAu);
    EXPECT_EQ(mem[1], 0u);
    EXPECT_EQ(mem[2], 0xBu);
    EXPECT_EQ(mem[3], 0xCu);
    EXPECT_EQ(mem[4], 0xDu);
    EXPECT_EQ(mem[5], 0xEu);
    EXPECT_NE(mem[6] | mem[7], 0u);
    EXPECT_EQ(f.interrupts, std::vector<std::uint32_t>(3, gfx_eop_interrupt));
    EXPECT_EQ(f.syncs, std::vector(6, SyncPoint::before_write));
}

TEST(MemoryPackets, WaitRegMemWaitsUntilTheCompareHolds) {
    Fixture f;
    std::uint32_t label = 0;
    std::vector<std::uint32_t> dcb;
    // The game's writer: memory space, poll interval 10. Waits for (label & 0xFF) >= 3.
    packet(dcb, IT_WAIT_REG_MEM, {0x15, lo(&label), hi(&label), 3, 0xFF, 10});
    packet(dcb, IT_SET_CONTEXT_REG, {0x300, 1});
    packet(dcb, IT_WAIT_REG_MEM, {0x03, 0xA300, 0, 1, 0xFFFFFFFF, 10});  // register == 1
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::waiting);
    label = 0x102;
    EXPECT_EQ(f.parser.run(stream), RunResult::waiting);
    EXPECT_EQ(f.syncs, std::vector(2, SyncPoint::before_wait));
    label = 0x3;
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    EXPECT_EQ(f.regs.get(0xA300), 1u);

    std::vector<std::uint32_t> reserved;
    packet(reserved, IT_WAIT_REG_MEM, {0x17, lo(&label), hi(&label), 0, 0, 10});
    EXPECT_EQ(f.run(reserved), RunResult::error);
}

TEST(MemoryPackets, MemSemaphoreSignalsAndWaits) {
    Fixture f;
    alignas(8) std::uint64_t semaphore = 0;
    const std::uint32_t hi_bits = hi(&semaphore);
    std::vector<std::uint32_t> wait;
    packet(wait, IT_MEM_SEMAPHORE, {lo(&semaphore), 0xE0000000 | hi_bits});
    std::vector<std::uint32_t> signal;
    packet(signal, IT_MEM_SEMAPHORE, {lo(&semaphore), 0xC0000000 | hi_bits});  // increment
    packet(signal, IT_MEM_SEMAPHORE, {lo(&semaphore), 0xC0000000 | hi_bits});
    std::vector<std::uint32_t> write_one;
    packet(write_one, IT_MEM_SEMAPHORE, {lo(&semaphore), 0xC0100000 | hi_bits});

    Stream waiting(wait);
    EXPECT_EQ(f.parser.run(waiting), RunResult::waiting);
    EXPECT_EQ(f.run(signal), RunResult::done);
    EXPECT_EQ(semaphore, 2u);
    EXPECT_EQ(f.parser.run(waiting), RunResult::done);
    EXPECT_EQ(semaphore, 1u);
    EXPECT_EQ(f.run(write_one), RunResult::done);
    EXPECT_EQ(f.run(write_one), RunResult::done);
    EXPECT_EQ(semaphore, 1u);
}

TEST(MemoryPackets, AccessesOutsideTheMappingsFail) {
    Fixture f;
    std::array<std::uint32_t, 2> mem{};
    f.parser.set_readable([&](const void* address, std::size_t bytes) {
        const auto* p = static_cast<const std::uint8_t*>(address);
        const auto* base = reinterpret_cast<const std::uint8_t*>(mem.data());
        return p >= base && p + bytes <= base + sizeof mem;
    });
    const auto fails = [&](std::uint8_t opcode, std::initializer_list<std::uint32_t> body) {
        std::vector<std::uint32_t> dcb;
        packet(dcb, opcode, body);
        return f.run(dcb) == RunResult::error;
    };
    EXPECT_TRUE(fails(IT_WRITE_DATA, {0x500, lo(&mem[1]), hi(&mem[1]), 1, 2}));
    const std::uint32_t* past = mem.data() + 2;
    EXPECT_TRUE(fails(IT_COPY_DATA, {0x501, lo(past), hi(past), lo(&mem[0]), hi(&mem[0])}));
    EXPECT_TRUE(fails(IT_DMA_DATA__CI__VI, {0, lo(&mem[0]), hi(&mem[0]), lo(&mem[1]), hi(&mem[1]),
                                            8}));
    EXPECT_TRUE(fails(IT_EVENT_WRITE_EOP, {0x504, lo(&mem[1]), 2u << 29 | hi(&mem[1]), 0, 0}));
    EXPECT_TRUE(fails(IT_WAIT_REG_MEM, {0x13, lo(past), hi(past), 0, 0, 10}));
    EXPECT_TRUE(fails(IT_RELEASE_MEM__CI__VI, {0x514, 1u << 29}));  // body too short
    EXPECT_FALSE(fails(IT_EVENT_WRITE_EOP, {0x504, lo(&mem[1]), 1u << 29 | hi(&mem[1]), 3, 0}));
    EXPECT_EQ(mem[1], 3u);
}

// A DCB waiting on a label runs on once the CPU writes it; an EOP interrupt reaches the GPU
// thread's handler.
TEST(MemoryPackets, GpuThreadResumesALabelWait) {
    std::atomic<int> interrupts{0};
    GpuThread gpu({}, [&](std::uint32_t) { interrupts.fetch_add(1); });
    alignas(8) std::array<std::uint32_t, 2> label{};
    std::vector<std::uint32_t> waiter;
    packet(waiter, IT_WAIT_REG_MEM, {0x13, lo(&label[0]), hi(&label[0]), 7, 0xFFFFFFFF, 10});
    packet(waiter, IT_SET_CONTEXT_REG, {0x207, 7});
    std::vector<std::uint32_t> writer;
    packet(writer, IT_EVENT_WRITE_EOP,
           {0x504, lo(&label[0]), 1u << 29 | 2u << 24 | hi(&label[0]), 7, 0});
    const std::array<std::span<const std::uint32_t>, 1> first{waiter};
    const std::array<std::span<const std::uint32_t>, 1> second{writer};

    gpu.submit(Queue::graphics, first);
    EXPECT_FALSE(gpu.wait_idle(20ms));
    std::atomic_ref(label[0]).store(7, std::memory_order_release);
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(gpu.registers().get(0xA207), 7u);
    EXPECT_EQ(interrupts.load(), 0);
    gpu.submit(Queue::graphics, second);
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(interrupts.load(), 1);
    EXPECT_EQ(gpu.stats().errors, 0u);
}

}  // namespace
