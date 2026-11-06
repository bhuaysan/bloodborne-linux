// The eboot facts behind bb-run's main-loop probe (src/tools/main_loop_probe.hpp).
#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "harness/eboot.hpp"
#include "tools/main_loop_probe.hpp"

namespace {

using bb::test::EbootTest;

TEST_F(EbootTest, FramePacerSizeIsItsFdeLength) {
    EXPECT_EQ(image().function_size(bb::tools::frame_pacer),
              std::optional<std::uint64_t>{bb::tools::frame_pacer_size});
}

}  // namespace
