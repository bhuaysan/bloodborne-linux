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

}  // namespace
