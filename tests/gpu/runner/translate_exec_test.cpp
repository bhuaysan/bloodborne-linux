// Execution tests of the translator on lavapipe: our GCN programs in tests/gpu/programs/, run as
// one workgroup of 64 lanes, against values worked out from the ISA manual. The modules are the
// ones that GcnTranslate.ProgramsPassSpirvVal validates (same programs, same key).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/runner/compute_runner.hpp"

namespace {

using namespace bb::test::vk;

constexpr std::uint64_t base = 0x12'0000'0000;  // guest address of the output
constexpr std::uint32_t lanes = 64;

class TranslateExec : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        runner_ = ComputeRunner::create(why);
        if (!runner_) {
            GTEST_SKIP() << why;
        }
    }

    // Runs the program with user_data = {output address, then `inputs`} and returns the first
    // `words` dwords of the output.
    std::vector<std::uint32_t> run(std::string_view name, std::vector<std::uint32_t> inputs,
                                   std::uint32_t words) {
        const auto translation =
            bb::gcn::translate(bb::test::gcn::program_words(name), {}, {.pgm_rsrc2 = 16u << 1});
        auto memory = runner_->guest_memory(base, 0x1000);
        DrawBlock draw;
        draw.user_data = {static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32)};
        std::ranges::copy(inputs, draw.user_data.begin() + 2);
        runner_->run(translation.spirv, draw, {1, 1, 1});
        std::vector<std::uint32_t> out(words);
        std::memcpy(out.data(), memory.data(), words * 4);
        return out;
    }

    std::unique_ptr<ComputeRunner> runner_;
};

TEST_F(TranslateExec, ExecLaneBits) {
    constexpr std::uint32_t value = 0xa5a5'5a5a, exec_lo = 0x8000'00f1, exec_hi = 0xf0f0'f0f0;
    const auto out = run("exec_lanes", {value, exec_lo}, 2 * lanes);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        SCOPED_TRACE(testing::Message() << "lane " << i);
        const bool active = ((i < 32 ? exec_lo >> i : exec_hi >> (i - 32)) & 1) != 0;
        const std::uint32_t x = value ^ (4 * i);
        const std::uint32_t v6 = active ? x & 0xffff : x;
        EXPECT_EQ(out[i], active ? v6 : 0);           // stored by the active lanes only
        EXPECT_EQ(out[lanes + i], i >= 2 ? v6 : 0);  // EXEC = -4
    }
}

// The other writes to EXEC: from EXEC itself, the constants 0 and -1, a float
// constant (the f64 1.0 is 0x3ff0'0000'0000'0000, lanes 52-61), and a 32-bit ALU write of one
// half (EXEC = -1 first, so the high half keeps lanes 32-63).
TEST_F(TranslateExec, ExecWrites) {
    constexpr std::uint32_t mask_lo = 0x0000'f00f, mask_hi = 0xc300'0001;
    constexpr std::uint64_t mask = (std::uint64_t{mask_hi} << 32) | mask_lo;
    constexpr std::uint64_t stage_exec[] = {
        mask, 0, ~std::uint64_t{0}, 0x3ff0'0000ull << 32, (~std::uint64_t{0} << 32) | 0x111,
    };
    const auto out = run("exec_writes", {mask_lo, mask_hi, 0x100}, 5 * lanes);
    for (std::uint32_t stage = 0; stage < 5; ++stage) {
        for (std::uint32_t i = 0; i < lanes; ++i) {
            SCOPED_TRACE(testing::Message() << "stage " << stage << " lane " << i);
            const bool active = ((stage_exec[stage] >> i) & 1) != 0;
            EXPECT_EQ(out[stage * lanes + i], active ? 0x100 | i : 0u);
        }
    }
}

TEST_F(TranslateExec, ScalarLoopAndSkips) {
    const auto out = run("scalar_loop", {5, 0x8000'0001}, lanes + 1);
    for (std::uint32_t i = 0; i < lanes; ++i) {
        EXPECT_EQ(out[i], (5u + 4 + 3 + 2 + 1 + 64) ^ i) << "lane " << i;
    }
    EXPECT_EQ(out[lanes], 0u);
}

}  // namespace
