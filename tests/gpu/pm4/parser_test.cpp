#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/parser.hpp"

namespace {

using bb::pm4::Packet;
using bb::pm4::Parser;
using bb::pm4::RegisterMirror;
using bb::pm4::RegSpace;
using bb::pm4::RunResult;
using bb::pm4::Stream;
using bb::pm4::type3_header;

// Appends one type-3 packet.
void packet(std::vector<std::uint32_t>& out, std::uint8_t opcode,
            std::initializer_list<std::uint32_t> body) {
    out.push_back(type3_header(opcode, static_cast<std::uint32_t>(body.size())));
    out.insert(out.end(), body);
}

// An INDIRECT_BUFFER to `target`.
void indirect(std::vector<std::uint32_t>& out, const std::vector<std::uint32_t>& target,
              bool chain = false) {
    const auto address = reinterpret_cast<std::uint64_t>(target.data());
    packet(out, IT_INDIRECT_BUFFER,
           {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32),
            static_cast<std::uint32_t>(target.size()) | (chain ? 1u << 20 : 0u)});
}

struct Fixture {
    RegisterMirror regs;
    Parser parser{regs};
    std::vector<std::uint8_t> opcodes;  // of every packet that ran

    Fixture() {
        parser.set_trace([this](const Packet& p) { opcodes.push_back(p.opcode()); });
    }
};

TEST(Pm4Parser, SkipsFillerAndNops) {
    Fixture f;
    std::vector<std::uint32_t> dcb = {bb::pm4::type2_filler, type3_header(IT_NOP, 0)};
    packet(dcb, IT_NOP, {0x68750777, 0, 0});
    packet(dcb, IT_NOP, {0xC01E0000, 7});
    dcb.push_back(bb::pm4::type2_filler);
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    EXPECT_TRUE(stream.done());
    EXPECT_EQ(stream.position(), nullptr);
    EXPECT_EQ(f.opcodes, (std::vector<std::uint8_t>{IT_NOP, IT_NOP, IT_NOP}));
    Stream empty(std::span<const std::uint32_t>{});
    EXPECT_EQ(f.parser.run(empty), RunResult::done);
}

TEST(Pm4Parser, SetRegPacketsFillTheMirror) {
    Fixture f;
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_SET_CONFIG_REG, {0x10, 1});
    packet(dcb, IT_SET_SH_REG, {0x48, 2, 3});
    packet(dcb, IT_SET_CONTEXT_REG, {0xF0000202, 4});  // CI's index bits are ignored
    packet(dcb, IT_SET_UCONFIG_REG__CI__VI, {0x242, 5});
    packet(dcb, IT_SET_SH_REG, {0x3FF, 6, 7});  // the second value is past the SH space
    packet(dcb, IT_SET_CONTEXT_REG, {0x10});    // no values
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    EXPECT_EQ(f.regs.get(0x2010), 1u);
    EXPECT_EQ(f.regs.get(0x2C48), 2u);
    EXPECT_EQ(f.regs.get(0x2C49), 3u);
    EXPECT_EQ(f.regs.get(0xA202), 4u);
    EXPECT_EQ(f.regs.get(0xC242), 5u);
    EXPECT_EQ(f.regs.get(0x2FFF), 6u);
    EXPECT_EQ(f.regs.get(0x3000), 0u);
    EXPECT_EQ(f.opcodes.size(), 6u);
}

TEST(Pm4Parser, IndirectBuffersNestAndChain) {
    Fixture f;
    std::vector<std::uint32_t> inner;
    packet(inner, IT_SET_CONFIG_REG, {0x1, 0x22});
    std::vector<std::uint32_t> chained;
    packet(chained, IT_SET_CONFIG_REG, {0x2, 0x33});
    std::vector<std::uint32_t> ib;
    indirect(ib, inner);
    indirect(ib, chained, true);
    packet(ib, IT_SET_CONFIG_REG, {0x3, 0x44});  // never runs: the chain replaced this buffer
    std::vector<std::uint32_t> dcb;
    indirect(dcb, ib);
    packet(dcb, IT_SET_CONFIG_REG, {0x4, 0x55});  // runs after the indirect buffer
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    EXPECT_EQ(f.regs.get(0x2001), 0x22u);
    EXPECT_EQ(f.regs.get(0x2002), 0x33u);
    EXPECT_EQ(f.regs.get(0x2003), 0u);
    EXPECT_EQ(f.regs.get(0x2004), 0x55u);
}

TEST(Pm4Parser, IndirectBuffersNestAtMostFourDeep) {
    Fixture f;
    std::array<std::vector<std::uint32_t>, 5> buffers;
    packet(buffers[4], IT_NOP, {0});
    for (int i = 3; i >= 0; --i) {
        indirect(buffers[i], buffers[i + 1]);
    }
    Stream stream(buffers[0]);
    EXPECT_EQ(f.parser.run(stream), RunResult::error);
    EXPECT_EQ(stream.position(), buffers[3].data());  // stays at the fourth INDIRECT_BUFFER
}

TEST(Pm4Parser, UnreadableIndirectBufferFails) {
    Fixture f;
    f.parser.set_readable([](const void*, std::size_t) { return false; });
    const std::vector<std::uint32_t> target = {bb::pm4::type2_filler};
    std::vector<std::uint32_t> dcb;
    indirect(dcb, target);
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::error);
    EXPECT_EQ(stream.position(), dcb.data());
}

TEST(Pm4Parser, RewindWaitsAndResumesWhereItStopped) {
    Fixture f;
    std::vector<std::uint32_t> ib;
    packet(ib, IT_SET_CONFIG_REG, {0x1, 0x11});
    packet(ib, IT_REWIND__CI__VI, {0});
    packet(ib, IT_SET_CONFIG_REG, {0x2, 0x22});
    std::vector<std::uint32_t> dcb;
    indirect(dcb, ib);
    packet(dcb, IT_SET_CONFIG_REG, {0x3, 0x33});
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::waiting);
    EXPECT_EQ(stream.position(), ib.data() + 3);
    EXPECT_EQ(f.parser.run(stream), RunResult::waiting);
    EXPECT_EQ(f.regs.get(0x2001), 0x11u);
    EXPECT_EQ(f.regs.get(0x2002), 0u);

    std::atomic_ref(ib[4]).store(0x80000000, std::memory_order_release);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    EXPECT_EQ(f.regs.get(0x2002), 0x22u);
    EXPECT_EQ(f.regs.get(0x2003), 0x33u);
    EXPECT_EQ(f.opcodes, (std::vector<std::uint8_t>{IT_INDIRECT_BUFFER, IT_SET_CONFIG_REG,
                                                    IT_REWIND__CI__VI, IT_SET_CONFIG_REG,
                                                    IT_SET_CONFIG_REG}));
}

TEST(Pm4Parser, ContextControlAndClearState) {
    Fixture f;
    const std::array<std::uint32_t, 2> defaults = {0, 0xCC0010};
    f.regs.set_context_defaults(defaults);
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_SET_CONTEXT_REG, {0x1, 0x77});
    packet(dcb, IT_CONTEXT_CONTROL, {0x80000000, 0x80000001});
    packet(dcb, IT_CLEAR_STATE, {0});
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    EXPECT_EQ(f.regs.get(0xA001), 0xCC0010u);
    EXPECT_EQ(f.parser.context_control(), (std::array<std::uint32_t, 2>{0x80000000, 0x80000001}));
}

TEST(Pm4Parser, SkipsUnknownOpcodesAndType0) {
    Fixture f;
    std::vector<std::uint32_t> dcb;
    packet(dcb, 0x8C, {1, 0x420, 0, 0, 0});
    packet(dcb, IT_DRAW_INDEX_AUTO, {3, 2});  // known, run by a later item
    dcb.insert(dcb.end(), {0x00010100, 1, 2});  // type 0: two registers
    packet(dcb, IT_SET_CONFIG_REG, {0x1, 0x11});
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    EXPECT_EQ(f.regs.get(0x2001), 0x11u);
    EXPECT_EQ(f.regs.get(0x2100), 0u);
    EXPECT_FALSE(Parser::known_opcode(0x8C));
    EXPECT_FALSE(Parser::known_opcode(0x8D));
    EXPECT_TRUE(Parser::known_opcode(IT_EVENT_WRITE_EOP));
}

TEST(Pm4Parser, MalformedPacketsStopTheStream) {
    const std::vector<std::vector<std::uint32_t>> cases = {
        {type3_header(IT_NOP, 3), 0},                 // runs past the buffer
        {0x40000000},                                 // type 1
        {type3_header(IT_SET_SH_REG, 0)},             // no register offset
        {type3_header(IT_INDIRECT_BUFFER, 2), 0, 0},  // no size dword
        {type3_header(IT_CONTEXT_CONTROL, 1), 0},     // one dword short
        {0x00020100, 0},                              // type 0 past the buffer
    };
    for (const auto& body : cases) {
        Fixture f;
        std::vector<std::uint32_t> dcb = {bb::pm4::type2_filler};
        dcb.insert(dcb.end(), body.begin(), body.end());
        Stream stream(dcb);
        EXPECT_EQ(f.parser.run(stream), RunResult::error) << std::hex << body[0];
        EXPECT_EQ(stream.position(), dcb.data() + 1) << std::hex << body[0];
        EXPECT_EQ(f.parser.run(stream), RunResult::error);
    }
}

TEST(Pm4Parser, DispatchesReachTheCallbackWithTheRegistersTheyRunWith) {
    Fixture f;
    std::vector<bb::pm4::DispatchCall> calls;
    std::vector<std::uint32_t> counts;  // COMPUTE_NUM_THREAD_X at each call
    f.parser.set_dispatch([&](const RegisterMirror& regs, const bb::pm4::DispatchCall& call) {
        calls.push_back(call);
        counts.push_back(regs.num_threads(0));
    });
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_SET_SH_REG, {0x207, 64});  // COMPUTE_NUM_THREAD_X
    packet(dcb, IT_DISPATCH_DIRECT, {4, 5, 6, 1});
    packet(dcb, IT_DISPATCH_DIRECT, {1, 1, 1, 0});  // COMPUTE_SHADER_EN clear: skipped
    packet(dcb, IT_SET_SH_REG, {0x207, 32});
    packet(dcb, IT_SET_BASE, {0, 0x1111, 0});  // another base index: ignored
    packet(dcb, IT_SET_BASE, {1, 0xAAAA0000u, 0x12345});  // the address keeps 48 bits
    packet(dcb, IT_DISPATCH_INDIRECT, {0x10, 1});
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].groups, (std::array<std::uint32_t, 3>{4, 5, 6}));
    EXPECT_EQ(calls[0].indirect, 0u);
    EXPECT_EQ(calls[1].indirect, 0x2345AAAA0010u);
    EXPECT_EQ(counts, (std::vector<std::uint32_t>{64, 32}));
}

TEST(Pm4Parser, IndirectDispatchNeedsABaseAndShortPacketsFail) {
    const std::vector<std::vector<std::uint32_t>> cases = {
        {type3_header(IT_DISPATCH_INDIRECT, 2), 0, 1},  // no SET_BASE yet
        {type3_header(IT_DISPATCH_DIRECT, 3), 1, 1, 1},  // no initiator
        {type3_header(IT_DISPATCH_INDIRECT, 1), 0},      // no initiator
        {type3_header(IT_SET_BASE, 2), 1, 0},            // no address high
    };
    for (const auto& body : cases) {
        Fixture f;
        int calls = 0;
        f.parser.set_dispatch([&](const RegisterMirror&, const bb::pm4::DispatchCall&) { ++calls; });
        Stream stream(body);
        EXPECT_EQ(f.parser.run(stream), RunResult::error) << std::hex << body[0];
        EXPECT_EQ(calls, 0);
    }
}

TEST(Pm4Parser, ComputeRingIndirectDispatchCarriesItsAddress) {
    Fixture f;
    std::vector<bb::pm4::DispatchCall> calls;
    f.parser.set_dispatch(
        [&](const RegisterMirror&, const bb::pm4::DispatchCall& call) { calls.push_back(call); });
    std::vector<std::uint32_t> ring;
    packet(ring, IT_DISPATCH_INDIRECT, {0xAAAA0010u, 0x12345, 1});  // no SET_BASE
    packet(ring, IT_DISPATCH_INDIRECT, {0x1000, 0, 0});  // COMPUTE_SHADER_EN clear
    Stream stream(ring, bb::pm4::Engine::compute);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].indirect, 0x2345AAAA0010u);
}

TEST(Pm4Parser, IndirectBaseBelongsToItsStream) {
    Fixture f;
    int calls = 0;
    f.parser.set_dispatch([&](const RegisterMirror&, const bb::pm4::DispatchCall&) { ++calls; });
    std::vector<std::uint32_t> first;
    packet(first, IT_SET_BASE, {1, 0x1000, 0});
    packet(first, IT_DISPATCH_INDIRECT, {0, 1});
    std::vector<std::uint32_t> second;
    packet(second, IT_DISPATCH_INDIRECT, {0, 1});
    Stream a(first);
    EXPECT_EQ(f.parser.run(a), RunResult::done);
    Stream b(second);
    EXPECT_EQ(f.parser.run(b), RunResult::error);
    EXPECT_EQ(calls, 1);
}

TEST(Pm4Parser, IndirectDispatchWithUnreadableCountsFails) {
    Fixture f;
    int calls = 0;
    f.parser.set_dispatch([&](const RegisterMirror&, const bb::pm4::DispatchCall&) { ++calls; });
    f.parser.set_readable([](const void*, std::size_t) { return false; });
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_SET_BASE, {1, 0x1000, 0});
    packet(dcb, IT_DISPATCH_INDIRECT, {0, 1});
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::error);
    EXPECT_EQ(calls, 0);
}

TEST(Pm4Parser, DispatchesWithoutACallbackAreSkipped) {
    Fixture f;
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
}

// Runs `dcb` with a draw callback and returns the draws.
std::vector<bb::pm4::DrawCall> run_draws(const std::vector<std::uint32_t>& dcb,
                                         RunResult expect = RunResult::done,
                                         std::vector<std::uint32_t>* regs_seen = nullptr) {
    Fixture f;
    std::vector<bb::pm4::DrawCall> calls;
    f.parser.set_draw([&](const RegisterMirror& regs, const bb::pm4::DrawCall& call) {
        calls.push_back(call);
        if (regs_seen != nullptr) {
            regs_seen->push_back(regs.num_threads(0));
        }
    });
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), expect);
    return calls;
}

TEST(Pm4Parser, DrawIndexAutoCarriesCountInstancesAndTheRegisters) {
    std::vector<std::uint32_t> dcb, regs_seen;
    packet(dcb, IT_SET_SH_REG, {0x207, 16});
    packet(dcb, IT_DRAW_INDEX_AUTO, {3, 2});
    packet(dcb, IT_NUM_INSTANCES, {5});
    packet(dcb, IT_SET_SH_REG, {0x207, 8});
    packet(dcb, IT_DRAW_INDEX_AUTO, {6, 2});
    packet(dcb, IT_DRAW_INDEX_AUTO, {0, 2});  // no vertices: nothing to draw
    packet(dcb, IT_NUM_INSTANCES, {0});
    packet(dcb, IT_DRAW_INDEX_AUTO, {6, 2});  // no instances: nothing to draw
    const auto calls = run_draws(dcb, RunResult::done, &regs_seen);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].kind, bb::pm4::DrawKind::index_auto);
    EXPECT_EQ(calls[0].index_count, 3u);
    EXPECT_EQ(calls[0].instances, 1u);  // the default
    EXPECT_EQ(calls[0].initiator, 2u);
    EXPECT_EQ(calls[1].index_count, 6u);
    EXPECT_EQ(calls[1].instances, 5u);
    EXPECT_EQ(regs_seen, (std::vector<std::uint32_t>{16, 8}));
}

TEST(Pm4Parser, DrawIndex2UsesItsOwnBaseAndTheIndexType) {
    const std::vector<std::uint16_t> indices(8, 0);
    const auto address = reinterpret_cast<std::uint64_t>(indices.data());
    std::vector<std::uint32_t> dcb;
    const std::uint32_t lo = static_cast<std::uint32_t>(address);
    const std::uint32_t hi = static_cast<std::uint32_t>(address >> 32);
    packet(dcb, IT_DRAW_INDEX_2, {8, lo, hi, 6, 0});
    packet(dcb, IT_INDEX_TYPE, {1});
    packet(dcb, IT_DRAW_INDEX_2, {8, lo, hi, 4, 0});  // 4 indices of 4 bytes: 16 bytes, readable
    const auto calls = run_draws(dcb);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].kind, bb::pm4::DrawKind::index_2);
    EXPECT_EQ(calls[0].index_address, address);
    EXPECT_EQ(calls[0].index_bytes, 2u);
    EXPECT_EQ(calls[0].index_count, 6u);
    EXPECT_EQ(calls[0].max_indices, 8u);
    EXPECT_EQ(calls[1].index_bytes, 4u);
}

TEST(Pm4Parser, DrawIndexOffset2ReadsIndexBaseAndFailsWithoutOne) {
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_DRAW_INDEX_OFFSET_2, {100, 0, 3, 0});
    EXPECT_TRUE(run_draws(dcb, RunResult::error).empty());  // no INDEX_BASE
    dcb.clear();
    packet(dcb, IT_INDEX_BASE, {0xAAAA0000u, 0x12345});  // the address keeps 48 bits
    packet(dcb, IT_NUM_INSTANCES, {2});
    packet(dcb, IT_DRAW_INDEX_OFFSET_2, {100, 7, 3, 0});
    const auto calls = run_draws(dcb);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].kind, bb::pm4::DrawKind::index_offset_2);
    EXPECT_EQ(calls[0].index_address, 0x2345AAAA0000u);
    EXPECT_EQ(calls[0].first_index, 7u);
    EXPECT_EQ(calls[0].index_count, 3u);
    EXPECT_EQ(calls[0].max_indices, 100u);
    EXPECT_EQ(calls[0].instances, 2u);
}

TEST(Pm4Parser, IndirectDrawsReadSetBasePlusOffsetAndTheIndexState) {
    const std::vector<std::uint32_t> args(5, 0);
    const auto address = reinterpret_cast<std::uint64_t>(args.data());
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_DRAW_INDIRECT, {0, 0x2C, 0x2D, 1});
    EXPECT_TRUE(run_draws(dcb, RunResult::error).empty());  // no SET_BASE
    dcb.clear();
    packet(dcb, IT_SET_BASE, {1, static_cast<std::uint32_t>(address - 4),
                              static_cast<std::uint32_t>(address >> 32)});
    packet(dcb, IT_DRAW_INDIRECT, {4, 0x2C, 0x2D, 1});
    packet(dcb, IT_INDEX_BASE, {0x1000, 0});
    packet(dcb, IT_INDEX_BUFFER_SIZE, {64});
    packet(dcb, IT_INDEX_TYPE, {1});
    packet(dcb, IT_DRAW_INDEX_INDIRECT, {4, 0x2E, 0x2F, 3});
    const auto calls = run_draws(dcb);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].kind, bb::pm4::DrawKind::indirect);
    EXPECT_EQ(calls[0].indirect, address);
    EXPECT_EQ(calls[0].base_vertex_reg, 0x2Cu);
    EXPECT_EQ(calls[0].start_instance_reg, 0x2Du);
    EXPECT_EQ(calls[0].initiator, 1u);
    EXPECT_EQ(calls[0].index_address, 0u);
    EXPECT_EQ(calls[1].kind, bb::pm4::DrawKind::index_indirect);
    EXPECT_EQ(calls[1].indirect, address);
    EXPECT_EQ(calls[1].index_address, 0x1000u);
    EXPECT_EQ(calls[1].max_indices, 64u);
    EXPECT_EQ(calls[1].index_bytes, 4u);
    EXPECT_EQ(calls[1].base_vertex_reg, 0x2Eu);
}

TEST(Pm4Parser, ShortDrawPacketsFailAndDrawsWithoutACallbackAreSkipped) {
    for (const std::uint8_t opcode : {IT_DRAW_INDEX_2, IT_DRAW_INDEX_AUTO, IT_DRAW_INDEX_OFFSET_2,
                                      IT_DRAW_INDEX_INDIRECT, IT_DRAW_INDIRECT, IT_INDEX_BASE,
                                      IT_INDEX_BUFFER_SIZE, IT_INDEX_TYPE, IT_NUM_INSTANCES}) {
        std::vector<std::uint32_t> dcb;
        packet(dcb, opcode, {});
        EXPECT_TRUE(run_draws(dcb, RunResult::error).empty()) << int{opcode};
    }
    Fixture f;
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_DRAW_INDEX_AUTO, {3, 2});
    Stream stream(dcb);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
}

TEST(Pm4Parser, IndexStateBelongsToItsStream) {
    std::vector<std::uint32_t> first, second;
    packet(first, IT_INDEX_BASE, {0x1000, 0});
    packet(first, IT_NUM_INSTANCES, {4});
    packet(second, IT_DRAW_INDEX_OFFSET_2, {10, 0, 3, 0});
    Fixture f;
    int calls = 0;
    f.parser.set_draw([&](const RegisterMirror&, const bb::pm4::DrawCall&) { ++calls; });
    Stream a(first), b(second);
    EXPECT_EQ(f.parser.run(a), RunResult::done);
    EXPECT_EQ(f.parser.run(b), RunResult::error);  // INDEX_BASE is not set in this stream
    EXPECT_EQ(calls, 0);
}

TEST(Pm4Parser, EmptyDrawsNeedNoIndexBufferAndAreNotReported) {
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_DRAW_INDEX_OFFSET_2, {10, 0, 0, 0});  // no indices, no INDEX_BASE
    packet(dcb, IT_DRAW_INDEX_2, {10, 0, 0, 0, 0});      // no indices, no address
    packet(dcb, IT_NUM_INSTANCES, {0});
    packet(dcb, IT_DRAW_INDEX_2, {10, 0, 0, 3, 0});
    EXPECT_TRUE(run_draws(dcb).empty());
}

TEST(Pm4Parser, UnreadableIndicesOrArgumentsFailTheStream) {
    const std::uint64_t indices = 0x10000;
    const std::uint64_t first_index_end = indices + 8 * 2 + 4 * 2;  // first index 8, 4 indices
    std::vector<std::uint32_t> indexed, offset, indirect_dcb;
    packet(indexed, IT_DRAW_INDEX_2, {10, static_cast<std::uint32_t>(indices), 0, 4, 0});
    packet(offset, IT_INDEX_BASE, {static_cast<std::uint32_t>(indices), 0});
    packet(offset, IT_DRAW_INDEX_OFFSET_2, {10, 8, 4, 0});
    packet(indirect_dcb, IT_SET_BASE, {1, 0x2000, 0});
    packet(indirect_dcb, IT_DRAW_INDIRECT, {0, 0x2C, 0x2D, 1});
    for (const auto* dcb : {&indexed, &offset, &indirect_dcb}) {
        Fixture f;
        int calls = 0;
        f.parser.set_draw([&](const RegisterMirror&, const bb::pm4::DrawCall&) { ++calls; });
        f.parser.set_readable([](const void*, std::size_t) { return false; });
        Stream stream(*dcb);
        EXPECT_EQ(f.parser.run(stream), RunResult::error);
        EXPECT_EQ(calls, 0);
    }
    // The checked range of DRAW_INDEX_OFFSET_2 starts at the first index and ends after the last.
    Fixture f;
    std::uint64_t begin = 0;
    std::size_t size = 0;
    f.parser.set_draw([](const RegisterMirror&, const bb::pm4::DrawCall&) {});
    f.parser.set_readable([&](const void* p, std::size_t n) {
        if (reinterpret_cast<std::uintptr_t>(p) >= indices) {
            begin = reinterpret_cast<std::uintptr_t>(p);
            size = n;
        }
        return true;
    });
    Stream stream(offset);
    EXPECT_EQ(f.parser.run(stream), RunResult::done);
    EXPECT_EQ(begin, indices + 16);
    EXPECT_EQ(begin + size, first_index_end);
}

TEST(Pm4Parser, IndexedDrawsWithAnUnsupportedIndexTypeFail) {
    std::vector<std::uint32_t> dcb;
    packet(dcb, IT_INDEX_TYPE, {2});  // 8-bit
    packet(dcb, IT_DRAW_INDEX_2, {10, 0x1000, 0, 3, 0});
    EXPECT_TRUE(run_draws(dcb, RunResult::error).empty());
    dcb.clear();
    packet(dcb, IT_INDEX_TYPE, {2});
    packet(dcb, IT_DRAW_INDEX_AUTO, {3, 0});  // not indexed: fine
    EXPECT_EQ(run_draws(dcb).size(), 1u);
}

}  // namespace
