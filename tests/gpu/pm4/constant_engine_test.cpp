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
using bb::pm4::GpuThread;
using bb::pm4::Parser;
using bb::pm4::Queue;
using bb::pm4::RegisterMirror;
using bb::pm4::RunResult;
using bb::pm4::Stream;
using bb::pm4::type3_header;
using namespace std::chrono_literals;

void packet(std::vector<std::uint32_t>& out, std::uint8_t opcode,
            std::initializer_list<std::uint32_t> body) {
    out.push_back(type3_header(opcode, static_cast<std::uint32_t>(body.size())));
    out.insert(out.end(), body);
}

std::array<std::uint32_t, 2> split(const void* address) {
    const auto value = reinterpret_cast<std::uint64_t>(address);
    return {static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32)};
}

// DUMP_CONST_RAM of `dwords` dwords from CE RAM byte offset `offset` to `dest`.
void dump(std::vector<std::uint32_t>& out, std::uint32_t offset, std::uint32_t dwords,
          const void* dest) {
    const auto [lo, hi] = split(dest);
    packet(out, IT_DUMP_CONST_RAM, {offset, dwords, lo, hi});
}

struct Fixture {
    RegisterMirror regs;
    Parser parser{regs};
};

TEST(ConstantEngine, WritesAndDumpsConstRam) {
    Fixture f;
    std::array<std::uint32_t, 4> dest{9, 9, 9, 9};
    std::vector<std::uint32_t> ccb;
    packet(ccb, IT_WRITE_CONST_RAM, {0x101, 0xA, 0xB, 0xC});  // bits 0-1 of the offset ignored
    packet(ccb, IT_WRITE_CONST_RAM, {0xBFFC, 0xE});           // the last dword
    dump(ccb, 0x104, 2, &dest[1]);
    dump(ccb, 0x100, 0, &dest[0]);  // nothing
    Stream stream(ccb, Engine::constant);
    EXPECT_EQ(stream.engine(), Engine::constant);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    const auto ram = f.parser.ce_ram();
    EXPECT_EQ(ram.size_bytes(), 0xC000u);
    EXPECT_EQ(ram[0x3F], 0u);
    EXPECT_EQ(ram[0x40], 0xAu);
    EXPECT_EQ(ram[0x42], 0xCu);
    EXPECT_EQ(ram[0x2FFF], 0xEu);
    EXPECT_EQ(dest, (std::array<std::uint32_t, 4>{9, 0xB, 0xC, 9}));
}

TEST(ConstantEngine, AccessesOutsideCeRamOrMappingsFail) {
    std::array<std::uint32_t, 4> dest{};
    std::vector<std::vector<std::uint32_t>> cases(6);
    packet(cases[0], IT_WRITE_CONST_RAM, {0xBFFC, 1, 2});  // one dword past CE RAM
    dump(cases[1], 0xBFF8, 3, dest.data());                // one dword past CE RAM
    dump(cases[2], 0, 1, dest.data());                     // unmapped (below)
    packet(cases[3], IT_DUMP_CONST_RAM, {0, 1, 0});        // no address high dword
    packet(cases[4], IT_WRITE_CONST_RAM, {});              // no offset
    packet(cases[5], IT_INCREMENT_CE_COUNTER, {});         // no counter select
    for (std::size_t i = 0; i < cases.size(); ++i) {
        Fixture f;
        f.parser.set_readable([&](const void* address, std::size_t) {
            return i != 2 || address != dest.data();
        });
        Stream stream(cases[i], Engine::constant);
        EXPECT_EQ(f.parser.run(stream), RunResult::error) << i;
        EXPECT_EQ(stream.position(), cases[i].data()) << i;
    }
    EXPECT_EQ(dest, (std::array<std::uint32_t, 4>{}));
}

// The DE waits at WAIT_ON_CE_COUNTER until the CE counter is ahead; the CE waits at
// WAIT_ON_DE_COUNTER_DIFF while it is that many increments ahead.
TEST(ConstantEngine, CountersOrderTheEngines) {
    Fixture f;
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_WAIT_ON_CE_COUNTER, {1});
    packet(dcb, IT_SET_CONFIG_REG, {0x1, 1});
    packet(dcb, IT_INCREMENT_DE_COUNTER, {1});
    packet(dcb, IT_WAIT_ON_CE_COUNTER, {1});
    packet(dcb, IT_SET_CONFIG_REG, {0x2, 2});
    std::vector<std::uint32_t> ccb;
    packet(ccb, IT_INCREMENT_CE_COUNTER, {2});  // the CS partition counter only
    packet(ccb, IT_INCREMENT_CE_COUNTER, {1});
    packet(ccb, IT_INCREMENT_CE_COUNTER, {3});
    packet(ccb, IT_WAIT_ON_DE_COUNTER_DIFF, {2});
    packet(ccb, IT_WRITE_CONST_RAM, {0, 5});
    Stream de(dcb);
    Stream ce(ccb, Engine::constant);

    EXPECT_EQ(f.parser.run(de), RunResult::waiting);
    EXPECT_EQ(de.position(), dcb.data());
    EXPECT_EQ(f.parser.run(ce), RunResult::waiting);  // two ahead
    EXPECT_EQ(f.parser.ce_counter(), 2u);
    EXPECT_EQ(f.parser.ce_ram()[0], 0u);
    EXPECT_EQ(f.parser.run(de), RunResult::done);  // CE at 2, DE at 1
    EXPECT_EQ(f.regs.get(0x2002), 2u);
    EXPECT_EQ(f.parser.de_counter(), 1u);
    EXPECT_EQ(f.parser.run(ce), RunResult::done);
    EXPECT_EQ(f.parser.ce_ram()[0], 5u);
}

TEST(ConstantEngine, EnginesSkipEachOthersPackets) {
    Fixture f;
    std::vector<std::uint32_t> ib;
    packet(ib, IT_WRITE_CONST_RAM, {0x8, 3});
    packet(ib, IT_SET_CONFIG_REG, {0x1, 1});  // a DE packet
    packet(ib, IT_INCREMENT_DE_COUNTER, {1});
    packet(ib, IT_WRITE_DATA, {0x84000000, 0, 0, 0});  // CE WRITE_DATA to no register
    packet(ib, IT_REWIND__CI__VI, {0x80000000});
    packet(ib, 0x8C, {0});  // unknown
    std::vector<std::uint32_t> ccb;
    const auto [lo, hi] = split(ib.data());
    packet(ccb, IT_INDIRECT_BUFFER_CNST, {lo, hi, static_cast<std::uint32_t>(ib.size())});
    packet(ccb, IT_NOP, {0});
    packet(ccb, IT_INDIRECT_BUFFER, {lo, hi, static_cast<std::uint32_t>(ib.size())});  // skipped
    Stream ce(ccb, Engine::constant);
    EXPECT_EQ(f.parser.run(ce), RunResult::done);
    EXPECT_EQ(f.parser.ce_ram()[2], 3u);
    EXPECT_EQ(f.regs.get(0x2001), 0u);
    EXPECT_EQ(f.parser.de_counter(), 0u);

    // The game's INDIRECT_BUFFER_CONST sets bits 20, 23 and 24: a chain.
    std::vector<std::uint32_t> chained;
    packet(chained, IT_INDIRECT_BUFFER_CNST,
           {lo, hi, static_cast<std::uint32_t>(ib.size()) | 0x1900000});
    packet(chained, IT_WRITE_CONST_RAM, {0x8, 4});  // replaced by the chain, never runs
    Stream chain(chained, Engine::constant);
    EXPECT_EQ(f.parser.run(chain), RunResult::done);
    EXPECT_EQ(f.parser.ce_ram()[2], 3u);

    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_WRITE_CONST_RAM, {0x8, 7});
    packet(dcb, IT_INCREMENT_CE_COUNTER, {1});
    packet(dcb, IT_INDIRECT_BUFFER_CNST, {lo, hi, static_cast<std::uint32_t>(ib.size())});
    Stream de(dcb);
    EXPECT_EQ(f.parser.run(de), RunResult::done);
    EXPECT_EQ(f.parser.ce_ram()[2], 3u);
    EXPECT_EQ(f.parser.ce_counter(), 0u);
}

// A DCB that waits on the CE counter runs once the CCB submitted after it has counted.
TEST(ConstantEngine, GpuThreadRunsTheCcbBesideTheDcb) {
    GpuThread gpu;
    std::array<std::uint32_t, 2> dest{};
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_WAIT_ON_CE_COUNTER, {1});
    packet(dcb, IT_SET_CONTEXT_REG, {0x206, 6});
    packet(dcb, IT_INCREMENT_DE_COUNTER, {1});
    std::vector<std::uint32_t> ccb;
    packet(ccb, IT_WRITE_CONST_RAM, {0x40, 0x11, 0x22});
    dump(ccb, 0x40, 2, dest.data());
    packet(ccb, IT_INCREMENT_CE_COUNTER, {1});
    const std::array<std::span<const std::uint32_t>, 1> draws{dcb};
    const std::array<std::span<const std::uint32_t>, 1> constants{ccb};

    gpu.submit(Queue::graphics, draws);
    EXPECT_FALSE(gpu.wait_idle(20ms));
    gpu.submit(Queue::constant, constants);
    ASSERT_TRUE(gpu.wait_idle(5s));
    EXPECT_EQ(gpu.registers().get(0xA206), 6u);
    EXPECT_EQ(dest, (std::array<std::uint32_t, 2>{0x11, 0x22}));
    EXPECT_EQ(gpu.parser().ce_counter(), 1u);
    EXPECT_EQ(gpu.parser().de_counter(), 1u);
    EXPECT_EQ(gpu.stats().finished, 2u);
    EXPECT_EQ(gpu.stats().errors, 0u);
}

}  // namespace
