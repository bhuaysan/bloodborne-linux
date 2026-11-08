#include "gpu/pm4/gpu_trace.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/gpu_thread.hpp"

namespace {

using bb::pm4::GpuThread;
using bb::pm4::GpuTrace;
using bb::pm4::Queue;
using bb::pm4::type3_header;
using namespace std::chrono_literals;

void packet(std::vector<std::uint32_t>& out, std::uint8_t opcode,
            std::initializer_list<std::uint32_t> body) {
    out.push_back(type3_header(opcode, static_cast<std::uint32_t>(body.size())));
    out.insert(out.end(), body);
}

std::filesystem::path temp_path(const char* name) {
    const std::filesystem::path path = std::filesystem::path(testing::TempDir()) / name;
    std::filesystem::remove(path);
    return path;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// Sets the VS program address, then draws and dispatches.
std::vector<std::uint32_t> frame_buffer() {
    std::vector<std::uint32_t> out;
    packet(out, IT_SET_SH_REG, {0x48, 0x1234, 0});  // SPI_SHADER_PGM_LO_VS, HI
    packet(out, IT_DRAW_INDEX_AUTO, {3, 2});
    packet(out, IT_DISPATCH_DIRECT, {4, 5, 6, 1});
    return out;
}

TEST(GpuTrace, DescribesOnlyDrawsAndDispatches) {
    bb::pm4::RegisterMirror regs;
    std::vector<std::uint32_t> buf;
    packet(buf, IT_NOP, {0});
    const bb::pm4::Packet nop{buf[0], std::span(buf).subspan(1)};
    EXPECT_TRUE(GpuTrace::describe(nop, regs).empty());
}

TEST(GpuTrace, WritesPacketsDrawsAndDispatchesPerFrame) {
    const auto path = temp_path("bb_gpu_trace_a.txt");
    auto trace = GpuTrace::open(path);
    ASSERT_NE(trace, nullptr);
    EXPECT_EQ(GpuTrace::open(path), nullptr);  // does not overwrite
    {
        GpuThread gpu({}, {}, trace.get());
        const auto buffer = frame_buffer();
        const std::span<const std::uint32_t> buffers[] = {buffer};
        gpu.submit(Queue::graphics, buffers, true);
        gpu.submit(Queue::graphics, buffers, true);
        ASSERT_TRUE(gpu.wait_idle(5s));
    }
    EXPECT_EQ(trace->frames(), 2u);
    trace.reset();
    const std::string text = read(path);
    EXPECT_NE(text.find("frame 1: draw op=0x2d body=2 0x3 0x2 vs=0x123400 ps=0x0"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("frame 2: dispatch op=0x15 body=4 0x4 0x5 0x6 0x1"), std::string::npos)
        << text;
    EXPECT_NE(text.find("frame 1 end: 3 packets, 1 draws, 1 dispatches; opcodes: 0x15x1 0x2dx1"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("frame 2 end: 3 packets"), std::string::npos) << text;
}

TEST(GpuTrace, NoFrameEndWithoutTheMark) {
    const auto path = temp_path("bb_gpu_trace_b.txt");
    auto trace = GpuTrace::open(path);
    ASSERT_NE(trace, nullptr);
    GpuThread gpu({}, {}, trace.get());
    const auto buffer = frame_buffer();
    const std::span<const std::uint32_t> buffers[] = {buffer};
    gpu.submit(Queue::graphics, buffers);
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(trace->frames(), 0u);
}

}  // namespace
