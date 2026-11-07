#include "gpu/pm4/gpu_thread.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

using bb::pm4::GpuThread;
using bb::pm4::Queue;
using bb::pm4::type3_header;
using namespace std::chrono_literals;

void packet(std::vector<std::uint32_t>& out, std::uint8_t opcode,
            std::initializer_list<std::uint32_t> body) {
    out.push_back(type3_header(opcode, static_cast<std::uint32_t>(body.size())));
    out.insert(out.end(), body);
}

// A stream that sets context register 0xA000 + offset to value.
std::vector<std::uint32_t> set_context(std::uint32_t offset, std::uint32_t value) {
    std::vector<std::uint32_t> out;
    packet(out, IT_SET_CONTEXT_REG, {offset, value});
    return out;
}

// A stream that waits at a REWIND until bit 31 of dword 1 is set, then sets 0xA001 to 7.
std::vector<std::uint32_t> rewind_then_set() {
    std::vector<std::uint32_t> out;
    packet(out, IT_REWIND__CI__VI, {0});
    packet(out, IT_SET_CONTEXT_REG, {1, 7});
    return out;
}

void submit(GpuThread& gpu, std::initializer_list<std::span<const std::uint32_t>> buffers) {
    gpu.submit(Queue::graphics, std::span(buffers.begin(), buffers.size()));
}

TEST(GpuThread, StartsWithTheClearStateDefaults) {
    GpuThread gpu;
    EXPECT_EQ(gpu.registers().get(0xA0D4), 0x2a00161au);
    EXPECT_EQ(gpu.registers().get(0xA317), 0x10u);
    std::vector<std::uint32_t> clear = set_context(0x317, 3);
    packet(clear, IT_CLEAR_STATE, {0});
    submit(gpu, {clear});
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(gpu.registers().get(0xA317), 0x10u);
}

TEST(GpuThread, RunsStreamsInSubmissionOrder) {
    GpuThread gpu;
    const auto first = set_context(0x202, 1);
    const auto second = set_context(0x202, 2);
    const auto third = set_context(0x203, 3);
    submit(gpu, {first, second});
    submit(gpu, {third});
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(gpu.registers().get(0xA202), 2u);
    EXPECT_EQ(gpu.registers().get(0xA203), 3u);
    const GpuThread::Stats stats = gpu.stats();
    EXPECT_EQ(stats.submitted, 3u);
    EXPECT_EQ(stats.finished, 3u);
    EXPECT_EQ(stats.errors, 0u);
    EXPECT_EQ(stats.packets, 3u);
}

TEST(GpuThread, DropsAFailingStreamAndRunsTheNext) {
    GpuThread gpu;
    const std::vector<std::uint32_t> type1{1u << 30};
    std::vector<std::uint32_t> truncated;
    packet(truncated, IT_SET_CONTEXT_REG, {0x202, 9});
    truncated.pop_back();
    const auto good = set_context(0x204, 4);
    submit(gpu, {type1, truncated, good});
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(gpu.registers().get(0xA204), 4u);
    EXPECT_EQ(gpu.registers().get(0xA202), 0u);
    EXPECT_EQ(gpu.stats().finished, 3u);
    EXPECT_EQ(gpu.stats().errors, 2u);
}

TEST(GpuThread, CheckedIndirectBufferIntoUnreadableMemoryFails) {
    GpuThread gpu([](const void*, std::size_t) { return false; });
    const std::array<std::uint32_t, 1> target{0x80000000};  // type-2 filler
    const auto address = reinterpret_cast<std::uint64_t>(target.data());
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_INDIRECT_BUFFER,
           {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32), 1});
    submit(gpu, {dcb});
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(gpu.stats().errors, 1u);
}

// A waiting stream holds back the streams after it in its queue, and resumes once its condition
// holds, polled without a new submission.
TEST(GpuThread, WaitingStreamResumesWhenReleased) {
    GpuThread gpu;
    auto waiting = rewind_then_set();
    const auto after = set_context(0x205, 5);
    submit(gpu, {waiting, after});
    EXPECT_FALSE(gpu.wait_idle(20ms));
    EXPECT_EQ(gpu.stats().finished, 0u);

    std::atomic_ref(waiting[1]).store(0x80000000, std::memory_order_release);
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(gpu.registers().get(0xA001), 7u);
    EXPECT_EQ(gpu.registers().get(0xA205), 5u);
    EXPECT_EQ(gpu.stats().finished, 2u);
    EXPECT_EQ(gpu.stats().errors, 0u);
}

TEST(GpuThread, StopsWhileAStreamWaits) {
    const auto waiting = rewind_then_set();
    {
        GpuThread gpu;
        submit(gpu, {waiting});
        EXPECT_FALSE(gpu.wait_idle(5ms));
    }  // the destructor returns although the stream never ends
}

}  // namespace
