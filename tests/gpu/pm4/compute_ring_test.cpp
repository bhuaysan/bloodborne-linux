// Compute rings on the GPU thread: doorbells, wrap-around, read-pointer write-back, nested
// INDIRECT_BUFFERs, interrupt sources and unmapping.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/gpu_thread.hpp"

namespace {

using bb::pm4::GpuThread;
using bb::pm4::type3_header;
using namespace std::chrono_literals;

constexpr std::uint32_t ring_dwords = 16;

// Writes one type-3 packet into the ring at `at` and returns the offset after it.
std::uint32_t put(std::span<std::uint32_t> ring, std::uint32_t at, std::uint8_t opcode,
                  std::initializer_list<std::uint32_t> body) {
    ring[at++] = type3_header(opcode, static_cast<std::uint32_t>(body.size()));
    for (const std::uint32_t dword : body) {
        ring[at++] = dword;
    }
    return at;
}

// A 4-dword packet: SET_CONTEXT_REG of 0xA000 + offset to value (and the next register to 0).
std::uint32_t put_set(std::span<std::uint32_t> ring, std::uint32_t at, std::uint32_t offset,
                      std::uint32_t value) {
    return put(ring, at, IT_SET_CONTEXT_REG, {offset, value, 0});
}

std::uint32_t read_offset(const std::uint32_t& read_pointer) {
    return std::atomic_ref(const_cast<std::uint32_t&>(read_pointer))
        .load(std::memory_order_acquire);
}

struct Fixture {
    Fixture() {
        read_pointer = 0x5A5A;
        index = gpu.map_ring(ring, &read_pointer, 3);
    }
    std::array<std::uint32_t, ring_dwords> ring{};
    std::uint32_t read_pointer = 0;
    GpuThread gpu;
    int index = -1;
};

TEST(ComputeRing, MapClearsTheReadPointer) {
    Fixture f;
    ASSERT_EQ(f.index, 0);
    EXPECT_EQ(read_offset(f.read_pointer), 0u);
    std::array<std::uint32_t, 12> odd{};
    std::uint32_t read_pointer = 0;
    EXPECT_EQ(f.gpu.map_ring(odd, &read_pointer, 0), -1);  // not a power of two
    EXPECT_EQ(f.gpu.map_ring(f.ring, &read_pointer, 0), 1);
}

TEST(ComputeRing, RunsUpToTheDoorbellAndWritesTheReadOffsetBack) {
    Fixture f;
    std::uint32_t at = put_set(f.ring, 0, 0x300, 1);
    at = put_set(f.ring, at, 0x302, 2);
    put_set(f.ring, at, 0x304, 3);  // written but not yet reported
    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 8));
    ASSERT_TRUE(f.gpu.wait_idle(5s));
    EXPECT_EQ(read_offset(f.read_pointer), 8u);
    EXPECT_EQ(f.gpu.registers().get(0xA302), 2u);
    EXPECT_EQ(f.gpu.registers().get(0xA304), 0u);

    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 8));  // nothing new
    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 12));
    ASSERT_TRUE(f.gpu.wait_idle(5s));
    EXPECT_EQ(read_offset(f.read_pointer), 12u);
    EXPECT_EQ(f.gpu.registers().get(0xA304), 3u);
    EXPECT_EQ(f.gpu.stats().submitted, 2u);
    EXPECT_EQ(f.gpu.stats().errors, 0u);
}

// The writer (0x108c830) wraps to the ring's start: the doorbell then reports a smaller offset,
// or 0 when the packets ended exactly at the ring's end.
TEST(ComputeRing, WrapsAroundTheEnd) {
    Fixture f;
    put_set(f.ring, 0, 0x300, 1);
    put_set(f.ring, 4, 0x300, 2);
    put_set(f.ring, 8, 0x300, 3);
    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 12));
    ASSERT_TRUE(f.gpu.wait_idle(5s));

    put_set(f.ring, 12, 0x302, 4);
    put_set(f.ring, 0, 0x300, 5);
    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 4));  // [12, 16) and [0, 4)
    ASSERT_TRUE(f.gpu.wait_idle(5s));
    EXPECT_EQ(read_offset(f.read_pointer), 4u);
    EXPECT_EQ(f.gpu.registers().get(0xA302), 4u);
    EXPECT_EQ(f.gpu.registers().get(0xA300), 5u);

    put_set(f.ring, 4, 0x300, 6);
    put_set(f.ring, 8, 0x300, 7);
    put_set(f.ring, 12, 0x300, 8);
    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 0));  // [4, 16)
    ASSERT_TRUE(f.gpu.wait_idle(5s));
    EXPECT_EQ(read_offset(f.read_pointer), 0u);
    EXPECT_EQ(f.gpu.registers().get(0xA300), 8u);
    EXPECT_EQ(f.gpu.stats().submitted, 4u);
    EXPECT_EQ(f.gpu.stats().errors, 0u);
}

// A failed stream is dropped whole: the read offset moves to its end, so the ring's free space
// doesn't shrink, and the next doorbell runs on.
TEST(ComputeRing, AFailedStreamMovesTheReadOffsetToItsEnd) {
    Fixture f;
    put_set(f.ring, 0, 0x300, 1);
    f.ring[4] = 1u << 30;  // a type-1 header fails the stream
    put_set(f.ring, 8, 0x300, 2);
    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 12));
    ASSERT_TRUE(f.gpu.wait_idle(5s));
    EXPECT_EQ(read_offset(f.read_pointer), 12u);
    EXPECT_EQ(f.gpu.registers().get(0xA300), 1u);
    EXPECT_EQ(f.gpu.stats().errors, 1u);

    put_set(f.ring, 12, 0x300, 3);
    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 0));
    ASSERT_TRUE(f.gpu.wait_idle(5s));
    EXPECT_EQ(read_offset(f.read_pointer), 0u);
    EXPECT_EQ(f.gpu.registers().get(0xA300), 3u);
    EXPECT_EQ(f.gpu.stats().errors, 1u);
}

// The ring holds INDIRECT_BUFFERs as the game writes them (0x108b330: shader-type bit, bits 23 and
// 24). One with the chain bit nests too, so the packet after it in the ring still runs. Its
// RELEASE_MEM interrupt reports the ring's source.
TEST(ComputeRing, FollowsIndirectBuffersAndReportsItsSource) {
    std::vector<std::uint32_t> interrupts;
    std::mutex mutex;
    std::array<std::uint32_t, ring_dwords> ring{};
    std::uint32_t read_pointer = 0;
    GpuThread gpu({}, [&](std::uint32_t source) {
        const std::scoped_lock lock(mutex);
        interrupts.push_back(source);
    });
    ASSERT_EQ(gpu.map_ring(ring, &read_pointer, 3), 0);

    std::array<std::uint32_t, 11> acb{};
    put(acb, put_set(acb, 0, 0x310, 9), IT_RELEASE_MEM__CI__VI, {0, 3u << 24, 0, 0, 0, 0});
    const auto address = reinterpret_cast<std::uint64_t>(acb.data());
    const std::uint32_t lo = static_cast<std::uint32_t>(address);
    const std::uint32_t hi = static_cast<std::uint32_t>(address >> 32) & 0xFFFF;
    std::uint32_t at = put(ring, 0, IT_INDIRECT_BUFFER, {lo, hi, 11 | 0x1800000});
    ring[0] |= 2;  // shader type: compute
    at = put(ring, at, IT_INDIRECT_BUFFER, {lo, hi, 11 | 0x1900000});
    put_set(ring, at, 0x312, 1);
    ASSERT_TRUE(gpu.ring_doorbell(0, 12));
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(read_offset(read_pointer), 12u);
    EXPECT_EQ(gpu.registers().get(0xA310), 9u);
    EXPECT_EQ(gpu.registers().get(0xA312), 1u);
    const std::scoped_lock lock(mutex);
    EXPECT_EQ(interrupts, (std::vector<std::uint32_t>{3, 3}));
}

// A waiting ring stream reports its position; unmapping drops it and the streams behind it and
// stops the write-back.
TEST(ComputeRing, WaitsAtItsPositionAndUnmapDropsTheRest) {
    Fixture f;
    std::uint32_t at = put_set(f.ring, 0, 0x300, 1);
    at = put(f.ring, at, IT_REWIND__CI__VI, {0});
    put_set(f.ring, at, 0x302, 2);
    ASSERT_TRUE(f.gpu.ring_doorbell(f.index, 10));
    EXPECT_FALSE(f.gpu.wait_idle(20ms));
    EXPECT_EQ(read_offset(f.read_pointer), 4u);
    EXPECT_EQ(f.gpu.registers().get(0xA300), 1u);

    EXPECT_FALSE(f.gpu.ring_doorbell(f.index, ring_dwords + 1));
    EXPECT_TRUE(f.gpu.unmap_ring(f.index));
    EXPECT_FALSE(f.gpu.unmap_ring(f.index));
    EXPECT_FALSE(f.gpu.ring_doorbell(f.index, 12));
    ASSERT_TRUE(f.gpu.wait_idle(5s));
    std::atomic_ref(f.ring[5]).store(0x80000000, std::memory_order_release);
    EXPECT_EQ(f.gpu.registers().get(0xA302), 0u);
    EXPECT_EQ(read_offset(f.read_pointer), 4u);
    EXPECT_EQ(f.gpu.stats().finished, 1u);
    EXPECT_EQ(f.gpu.stats().errors, 0u);
}

}  // namespace
