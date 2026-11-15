// The pad stream of bb-run's --pad-input (src/common/pad_stream.hpp): parsing and the mapping to the
// virtual joystick's buttons and axes. No SDL, GPU or device.

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/pad_stream.hpp"

namespace {

using bb::common::PadUpdate;
using bb::common::parse_pad_line;

TEST(PadStream, ButtonsMapToSdlsDefaultVirtualLayout) {
    EXPECT_EQ(parse_pad_line("button A 1"), (std::vector<PadUpdate>{{false, 0, 1}}));
    EXPECT_EQ(parse_pad_line("button A 0"), (std::vector<PadUpdate>{{false, 0, 0}}));
    EXPECT_EQ(parse_pad_line("button START 1"), (std::vector<PadUpdate>{{false, 6, 1}}));
    EXPECT_EQ(parse_pad_line("button RB 1"), (std::vector<PadUpdate>{{false, 10, 1}}));
}

TEST(PadStream, SticksKeepTheirRange) {
    EXPECT_EQ(parse_pad_line("axis LX -32768"), (std::vector<PadUpdate>{{true, 0, -32768}}));
    EXPECT_EQ(parse_pad_line("axis RY 32767"), (std::vector<PadUpdate>{{true, 3, 32767}}));
}

TEST(PadStream, TriggersScaleFromTheByteToTheAxisRange) {
    // SDL turns the axis range into the trigger's 0..32767, so a released trigger is the minimum.
    EXPECT_EQ(parse_pad_line("axis LT 0"), (std::vector<PadUpdate>{{true, 4, -32768}}));
    EXPECT_EQ(parse_pad_line("axis RT 255"), (std::vector<PadUpdate>{{true, 5, 32767}}));
}

TEST(PadStream, TheDpadPressesOneButtonAndReleasesTheOpposite) {
    EXPECT_EQ(parse_pad_line("axis HY 1"), (std::vector<PadUpdate>{{false, 11, 0}, {false, 12, 1}}));
    EXPECT_EQ(parse_pad_line("axis HY -1"), (std::vector<PadUpdate>{{false, 11, 1}, {false, 12, 0}}));
    EXPECT_EQ(parse_pad_line("axis HX -1"), (std::vector<PadUpdate>{{false, 13, 1}, {false, 14, 0}}));
    EXPECT_EQ(parse_pad_line("axis HX 0"), (std::vector<PadUpdate>{{false, 13, 0}, {false, 14, 0}}));
}

TEST(PadStream, NeutralReleasesEverything) {
    const std::vector<PadUpdate> updates = parse_pad_line("neutral");
    ASSERT_EQ(updates.size(), 15u + 6u);
    for (const PadUpdate& u : updates) {
        const bool trigger = u.is_axis && u.index >= 4;
        EXPECT_EQ(u.value, trigger ? -32768 : 0);
    }
    EXPECT_EQ(updates, bb::common::neutral_pad_updates());
}

TEST(PadStream, BlankLinesAndCommentsAreSkipped) {
    EXPECT_TRUE(parse_pad_line("").empty());
    EXPECT_TRUE(parse_pad_line("  \t\r").empty());
    EXPECT_TRUE(parse_pad_line("# tap A").empty());
    EXPECT_EQ(parse_pad_line("  button   B\t1\r"), (std::vector<PadUpdate>{{false, 1, 1}}));
}

TEST(PadStream, RefusesWhatItDoesNotKnow) {
    for (const char* line : {"button Z 1", "button A 2", "button A", "button A 1 1", "axis LX 32768",
                             "axis LT 256", "axis LT -1", "axis HX 2", "axis XX 0", "press A",
                             "neutral now", "button A x"}) {
        EXPECT_THROW(parse_pad_line(line), std::invalid_argument) << line;
    }
}

}  // namespace
