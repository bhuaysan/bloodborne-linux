// The default hardware state, marker and profiling query imports (src/hle/video/gnm_state.hpp):
// the dwords they write, that the parser runs the default state without unknown packets, and the
// failures.
#include "hle/video/gnm_state.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "hle/video/gnm_default_state.hpp"
#include "si_ci_vi_merged_pm4_it_opcodes.h"

namespace {

using namespace bb::hle::video;
using bb::pm4::default_state_dwords;
using Words = std::vector<std::uint32_t>;

constexpr std::uint32_t poison = 0xdeadbeef;

// What a walk over type-3 packets found: the opcodes in order, the last packet's body size and
// the registers that the SET_SH_REG, SET_CONTEXT_REG and SET_UCONFIG_REG packets wrote.
struct Walked {
    bool ok = false;  // every packet is type 3 and the last one ends exactly at the end
    std::vector<std::uint8_t> opcodes;
    std::size_t last_body = 0;
    std::uint32_t last_first = 0;  // the first body dword of the last packet
    std::map<std::uint32_t, std::uint32_t> regs;
};

Walked walk(std::span<const std::uint32_t> words) {
    Walked w;
    std::size_t i = 0;
    while (i < words.size()) {
        const std::uint32_t header = words[i];
        const std::size_t body = (header >> 16 & 0x3fff) + 1;
        if (header >> 30 != 3 || i + 1 + body > words.size()) {
            return w;
        }
        const auto opcode = static_cast<std::uint8_t>(header >> 8);
        w.opcodes.push_back(opcode);
        std::uint32_t base = 0;
        if (opcode == IT_SET_SH_REG) {
            base = 0x2c00;
        } else if (opcode == IT_SET_CONTEXT_REG) {
            base = 0xa000;
        } else if (opcode == IT_SET_UCONFIG_REG__CI__VI) {
            base = 0xc000;
        }
        if (base != 0) {
            for (std::size_t k = 1; k < body; ++k) {
                w.regs[base + words[i + 1] + static_cast<std::uint32_t>(k) - 1] = words[i + 1 + k];
            }
        }
        w.last_body = body;
        w.last_first = words[i + 1];
        i += 1 + body;
    }
    w.ok = true;
    return w;
}

TEST(GnmState, DrawDefaultStateFillsItsBlockAndParses) {
    Words buffer(default_state_dwords + 1, poison);
    ASSERT_EQ(sceGnmDrawInitDefaultHardwareState200(buffer.data(), default_state_dwords + 1),
              default_state_dwords);
    EXPECT_EQ(buffer.back(), poison);

    const auto state = bb::pm4::draw_default_state();
    ASSERT_EQ(state.size(), 130u);
    EXPECT_TRUE(std::equal(state.begin(), state.end(), buffer.begin()));

    const Walked walked = walk(std::span<const std::uint32_t>(buffer.data(), default_state_dwords));
    ASSERT_TRUE(walked.ok);
    // Exactly the opcodes of the default state: a stale entry fails too.
    const std::set<std::uint8_t> expected{IT_CONTEXT_CONTROL, IT_CLEAR_STATE, IT_ACQUIRE_MEM__CI__VI,
                                          IT_SET_SH_REG,      IT_SET_CONTEXT_REG, IT_NUM_INSTANCES,
                                          IT_SET_UCONFIG_REG__CI__VI, IT_NOP};
    EXPECT_EQ(std::set<std::uint8_t>(walked.opcodes.begin(), walked.opcodes.end()), expected);
    EXPECT_EQ(walked.opcodes.front(), IT_CONTEXT_CONTROL);
    EXPECT_EQ(walked.opcodes[1], IT_CLEAR_STATE);
    // The padding NOP: 125 body dwords, the first one 0.
    EXPECT_EQ(walked.opcodes.back(), IT_NOP);
    EXPECT_EQ(walked.last_body, 125u);
    EXPECT_EQ(walked.last_first, 0u);

    EXPECT_EQ(walked.regs.at(0xA202), 0xcc0010u);  // CB_COLOR_CONTROL
    EXPECT_EQ(walked.regs.at(0xA295), 0x100u);
    EXPECT_EQ(walked.regs.at(0xA297), 4u);
    EXPECT_EQ(walked.regs.at(0x2E16), 0xffffffffu);  // COMPUTE_STATIC_THREAD_MGMT_SE0
    EXPECT_EQ(walked.regs.at(0x2C47), 0x1cu);
    EXPECT_EQ(walked.regs.at(0xC200), 0xe0000000u);  // GRBM_GFX_INDEX
}

TEST(GnmState, DispatchDefaultStateFillsItsBlockAndParses) {
    Words buffer(default_state_dwords, poison);
    ASSERT_EQ(sceGnmDispatchInitDefaultHardwareState(buffer.data(), default_state_dwords),
              default_state_dwords);
    const Walked parsed = walk(buffer);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.opcodes,
              (std::vector<std::uint8_t>{IT_SET_SH_REG, IT_SET_SH_REG, IT_SET_SH_REG,
                                         IT_ACQUIRE_MEM__CI__VI, IT_NOP}));
    EXPECT_EQ(buffer[0], 0xc0017602u);  // shader-type bit set
    EXPECT_EQ(parsed.last_body, 239u);
    EXPECT_EQ(parsed.regs.at(0x2E15), 0x170u);  // COMPUTE_RESOURCE_LIMITS
    EXPECT_EQ(parsed.regs.at(0x2E17), 0xffffffffu);
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

    const Walked parsed = walk(std::span(buffer).first(13));
    EXPECT_TRUE(parsed.ok);
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
