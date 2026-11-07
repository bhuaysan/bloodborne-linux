// The default hardware state, marker and profiling query imports (src/hle/video/gnm_state.hpp):
// the dwords they write, that the parser runs the default state without unknown packets, and the
// failures.
#include "hle/video/gnm_state.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/default_state.hpp"
#include "gpu/pm4/parser.hpp"
#include "gpu/pm4/registers.hpp"

namespace {

using namespace bb::hle::video;
using bb::pm4::default_state_dwords;
using Words = std::vector<std::uint32_t>;

constexpr std::uint32_t poison = 0xdeadbeef;

struct Parsed {
    bb::pm4::RunResult result;
    std::vector<std::uint8_t> opcodes;
    bb::pm4::Packet last;
};

// Runs `words` on `regs`.
Parsed parse(std::span<const std::uint32_t> words, bb::pm4::RegisterMirror& regs) {
    bb::pm4::Parser parser(regs);
    Parsed parsed{};
    parser.set_trace([&](const bb::pm4::Packet& packet) {
        parsed.opcodes.push_back(packet.opcode());
        parsed.last = packet;
    });
    bb::pm4::Stream stream(words);
    parsed.result = parser.run(stream);
    return parsed;
}

TEST(GnmState, DrawDefaultStateFillsItsBlockAndParses) {
    Words buffer(default_state_dwords + 1, poison);
    ASSERT_EQ(sceGnmDrawInitDefaultHardwareState200(buffer.data(), default_state_dwords + 1),
              default_state_dwords);
    EXPECT_EQ(buffer.back(), poison);

    const auto state = bb::pm4::draw_default_state();
    ASSERT_EQ(state.size(), 130u);
    EXPECT_TRUE(std::equal(state.begin(), state.end(), buffer.begin()));

    bb::pm4::RegisterMirror regs;
    regs.write(bb::pm4::RegSpace::context, 0x300, Words{7});  // cleared by CLEAR_STATE
    const std::span<const std::uint32_t> block(buffer.data(), default_state_dwords);
    const Parsed parsed = parse(block, regs);
    ASSERT_EQ(parsed.result, bb::pm4::RunResult::done);
    for (const std::uint8_t opcode : parsed.opcodes) {
        EXPECT_TRUE(bb::pm4::Parser::known_opcode(opcode)) << std::hex << int{opcode};
    }
    EXPECT_EQ(parsed.opcodes.front(), IT_CONTEXT_CONTROL);
    EXPECT_EQ(parsed.opcodes[1], IT_CLEAR_STATE);
    // The padding NOP: 125 body dwords, the first one 0.
    EXPECT_EQ(parsed.last.opcode(), IT_NOP);
    ASSERT_EQ(parsed.last.body.size(), 125u);
    EXPECT_EQ(parsed.last.body[0], 0u);

    EXPECT_EQ(regs.get(0xA300), 0u);
    EXPECT_EQ(regs.get(0xA202), 0xcc0010u);    // CB_COLOR_CONTROL
    EXPECT_EQ(regs.get(0xA295), 0x100u);
    EXPECT_EQ(regs.get(0xA297), 4u);
    EXPECT_EQ(regs.get(0x2E16), 0xffffffffu);  // COMPUTE_STATIC_THREAD_MGMT_SE0
    EXPECT_EQ(regs.get(0x2C47), 0x1cu);
    EXPECT_EQ(regs.get(0xC200), 0xe0000000u);  // GRBM_GFX_INDEX
}

TEST(GnmState, DispatchDefaultStateFillsItsBlockAndParses) {
    Words buffer(default_state_dwords, poison);
    ASSERT_EQ(sceGnmDispatchInitDefaultHardwareState(buffer.data(), default_state_dwords),
              default_state_dwords);
    bb::pm4::RegisterMirror regs;
    const Parsed parsed = parse(buffer, regs);
    ASSERT_EQ(parsed.result, bb::pm4::RunResult::done);
    EXPECT_EQ(parsed.opcodes,
              (std::vector<std::uint8_t>{IT_SET_SH_REG, IT_SET_SH_REG, IT_SET_SH_REG,
                                         IT_ACQUIRE_MEM__CI__VI, IT_NOP}));
    EXPECT_EQ(buffer[0], 0xc0017602u);  // shader-type bit set
    EXPECT_EQ(parsed.last.body.size(), 239u);
    EXPECT_EQ(regs.get(0x2E15), 0x170u);  // COMPUTE_RESOURCE_LIMITS
    EXPECT_EQ(regs.get(0x2E17), 0xffffffffu);
}

TEST(GnmState, DefaultStateNeedsItsWholeBlock) {
    Words buffer(default_state_dwords, poison);
    EXPECT_EQ(sceGnmDrawInitDefaultHardwareState200(buffer.data(), default_state_dwords - 1), 0u);
    EXPECT_EQ(sceGnmDispatchInitDefaultHardwareState(buffer.data(), 16), 0u);
    EXPECT_EQ(sceGnmDrawInitDefaultHardwareState200(nullptr, default_state_dwords), 0u);
    EXPECT_TRUE(std::ranges::all_of(buffer, [](std::uint32_t w) { return w == poison; }));
}

TEST(GnmState, StringMarkersHoldTheirTagAndString) {
    EXPECT_EQ(marker_dwords(0), 6u);
    EXPECT_EQ(marker_dwords(3), 6u);
    EXPECT_EQ(marker_dwords(4), 9u);
    EXPECT_EQ(marker_dwords(13), 13u);

    const char* text = "Shadow pass 1";  // 13 bytes
    Words buffer(14, poison);
    ASSERT_EQ(sceGnmInsertPushMarker(buffer.data(), 13, text), 0);
    EXPECT_EQ(buffer[0], 0xc00b1000u);  // NOP with 12 body dwords
    EXPECT_EQ(buffer[1], marker_push_tag);
    EXPECT_EQ(std::memcmp(&buffer[2], text, 13), 0);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&buffer[2]);
    EXPECT_TRUE(std::all_of(bytes + 13, bytes + 11 * 4, [](unsigned char b) { return b == 0; }));
    EXPECT_EQ(buffer[13], poison);

    Words set(6, poison);
    ASSERT_EQ(sceGnmInsertSetMarker(set.data(), 6, ""), 0);
    EXPECT_EQ(set, (Words{0xc0041000u, marker_set_tag, 0, 0, 0, 0}));

    bb::pm4::RegisterMirror regs;
    const Parsed parsed = parse(std::span(buffer).first(13), regs);
    EXPECT_EQ(parsed.result, bb::pm4::RunResult::done);
    EXPECT_EQ(parsed.opcodes, std::vector<std::uint8_t>{IT_NOP});
}

TEST(GnmState, StringMarkersFailOnAWrongSize) {
    Words buffer(16, poison);
    EXPECT_EQ(sceGnmInsertPushMarker(buffer.data(), 7, "abc"), gnm_marker_error);
    EXPECT_EQ(sceGnmInsertSetMarker(buffer.data(), 5, "abc"), gnm_marker_error);
    EXPECT_EQ(sceGnmInsertSetMarker(buffer.data(), 6, nullptr), gnm_marker_error);
    EXPECT_EQ(sceGnmInsertPushMarker(nullptr, 6, "abc"), gnm_marker_error);
    EXPECT_TRUE(std::ranges::all_of(buffer, [](std::uint32_t w) { return w == poison; }));
}

TEST(GnmState, PopMarkerIsSixDwords) {
    Words buffer(7, poison);
    ASSERT_EQ(sceGnmInsertPopMarker(buffer.data(), pop_marker_dwords), 0);
    EXPECT_EQ(buffer, (Words{0xc0041000u, marker_pop_tag, 0, 0, 0, 0, poison}));
    EXPECT_EQ(sceGnmInsertPopMarker(buffer.data(), 5), gnm_marker_error);
    EXPECT_EQ(sceGnmInsertPopMarker(nullptr, 6), gnm_marker_error);
}

TEST(GnmState, ProfilingIsOff) {
    EXPECT_EQ(sceGnmIsUserPaEnabled(), 0);
    EXPECT_EQ(sceGnmDriverCaptureInProgress(), 0);
}

}  // namespace
