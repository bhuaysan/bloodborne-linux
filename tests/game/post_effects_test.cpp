// Tests of the post effect switches in src/game/gfx/post_effects.cpp (#665): each switch writes
// only its own fields of the block and the wrap gives the game's function the edited block and
// leaves the block as it was; against the eboot, the fields are the ones pfx_apply reads for the
// effect's YEBIS setters, and the wrap's prologue is the function's.
#include "game/gfx/post_effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "hook/hook.hpp"

namespace {

using bb::game::PostEffects;
using bb::test::EbootTest;

constexpr std::size_t block_dwords = 0x370 / 4;

std::uint32_t bits(float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, sizeof b);
    return b;
}
float as_float(std::uint32_t b) {
    float f = 0;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

// A block as the clinic's: every field a distinct marker, the effects' fields the game's values.
std::array<std::uint32_t, block_dwords> clinic_block() {
    std::array<std::uint32_t, block_dwords> block{};
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = 0xa5000000u + static_cast<std::uint32_t>(i);
    }
    block[0x2b] = 1;
    block[0x2c] = 3;
    block[0x2d] = bits(0.006f);
    block[0x2e] = bits(0.0f);
    block[0x2f] = bits(0.002f);
    block[0x30] = bits(-0.004f);
    block[0x77] = 1;
    block[0x78] = 2;
    block[0x79] = bits(0.095f);
    block[0x7a] = bits(0.2f);
    block[0xd4] = bits(4.0f);
    return block;
}

// The indices where `a` and `b` differ.
std::vector<std::size_t> changed(const std::array<std::uint32_t, block_dwords>& a,
                                 const std::array<std::uint32_t, block_dwords>& b) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            out.push_back(i);
        }
    }
    return out;
}

struct EffectsGuard {
    explicit EffectsGuard(const PostEffects& e) { bb::game::set_post_effects(e); }
    ~EffectsGuard() { bb::game::set_post_effects({}); }
};

TEST(PostEffects, TheDefaultChangesNothing) {
    auto block = clinic_block();
    const auto edit = bb::game::edit_post_effects({}, block.data());
    EXPECT_EQ(edit.count, 0u);
    EXPECT_TRUE(changed(block, clinic_block()).empty());
}

TEST(PostEffects, NoMotionBlurClearsItsEnableOnly) {
    auto block = clinic_block();
    PostEffects e;
    e.motion_blur = false;
    e.motion_blur_strength = 0.5f;  // no effect while the blur is off
    bb::game::edit_post_effects(e, block.data());
    EXPECT_EQ(changed(block, clinic_block()), std::vector<std::size_t>{0x77});
    EXPECT_EQ(block[0x77], 0u);
}

TEST(PostEffects, MotionBlurStrengthScalesTheTimeRatioOnly) {
    auto block = clinic_block();
    PostEffects e;
    e.motion_blur_strength = 0.5f;
    bb::game::edit_post_effects(e, block.data());
    EXPECT_EQ(changed(block, clinic_block()), std::vector<std::size_t>{0x79});
    EXPECT_EQ(as_float(block[0x79]), 0.095f * 0.5f);
}

TEST(PostEffects, NoChromaticAberrationClearsItsEnableOnly) {
    auto block = clinic_block();
    PostEffects e;
    e.chromatic_aberration = false;
    bb::game::edit_post_effects(e, block.data());
    EXPECT_EQ(changed(block, clinic_block()), std::vector<std::size_t>{0x2b});
    EXPECT_EQ(block[0x2b], 0u);
}

TEST(PostEffects, ChromaticAberrationStrengthScalesTheDispersions) {
    auto block = clinic_block();
    PostEffects e;
    e.chromatic_aberration_strength = 0.25f;
    bb::game::edit_post_effects(e, block.data());
    // 0x2e is 0 and stays 0.
    EXPECT_EQ(changed(block, clinic_block()), (std::vector<std::size_t>{0x2d, 0x2f, 0x30}));
    EXPECT_EQ(as_float(block[0x2d]), 0.006f * 0.25f);
    EXPECT_EQ(as_float(block[0x2f]), 0.002f * 0.25f);
    EXPECT_EQ(as_float(block[0x30]), -0.004f * 0.25f);
    EXPECT_EQ(block[0x2b], 1u);
}

TEST(PostEffects, BothSwitchesTogetherAndRestore) {
    auto block = clinic_block();
    PostEffects e;
    e.motion_blur = false;
    e.chromatic_aberration = false;
    const auto edit = bb::game::edit_post_effects(e, block.data());
    EXPECT_EQ(changed(block, clinic_block()), (std::vector<std::size_t>{0x2b, 0x77}));
    bb::game::restore_post_effects(edit, block.data());
    EXPECT_TRUE(changed(block, clinic_block()).empty());
}

TEST(PostEffects, SetRefusesStrengthsOutOfRange) {
    for (const float s : {-0.1f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()}) {
        PostEffects e;
        e.motion_blur_strength = s;
        EXPECT_THROW(bb::game::set_post_effects(e), std::invalid_argument) << s;
        e = {};
        e.chromatic_aberration_strength = s;
        EXPECT_THROW(bb::game::set_post_effects(e), std::invalid_argument) << s;
    }
    EXPECT_EQ(bb::game::post_effects(), PostEffects{});
}

// What the fake pfx_apply saw.
std::array<std::uint32_t, block_dwords> seen{};
void* seen_self = nullptr;
void* seen_context = nullptr;
void fake_apply(void* self, void* context, std::uint32_t* block) {
    seen_self = self;
    seen_context = context;
    std::copy_n(block, block_dwords, seen.begin());
}

TEST(PostEffects, TheWrapHandsOnTheEditedBlockAndRestoresIt) {
    PostEffects e;
    e.motion_blur = false;
    e.chromatic_aberration_strength = 0.5f;
    const EffectsGuard guard(e);
    auto block = clinic_block();
    int self = 0;
    int context = 0;
    // Twice on the same block: the strength scales the game's value, not the last call's.
    for (int call = 0; call < 2; ++call) {
        bb::game::pfx_apply_edited(&fake_apply, &self, &context, block.data());
        EXPECT_EQ(seen_self, &self);
        EXPECT_EQ(seen_context, &context);
        EXPECT_EQ(changed(seen, clinic_block()), (std::vector<std::size_t>{0x2d, 0x2f, 0x30, 0x77}));
        EXPECT_EQ(seen[0x77], 0u);
        EXPECT_EQ(as_float(seen[0x2d]), 0.003f);
        EXPECT_TRUE(changed(block, clinic_block()).empty());
    }
}

TEST(PostEffects, IsRegisteredWithItsPrologue) {
    auto all = bb::hook::replacements();
    auto it = std::find_if(all.begin(), all.end(), [](const bb::hook::Replacement& r) {
        return std::string_view(r.name) == "bb::game::pfx_apply";
    });
    ASSERT_NE(it, all.end());
    EXPECT_EQ(it->link_addr, bb::game::pfx_apply_link);
    ASSERT_EQ(it->prologue.size(), bb::game::pfx_apply_prologue.size());
    EXPECT_TRUE(std::equal(it->prologue.begin(), it->prologue.end(),
                           bb::game::pfx_apply_prologue.begin()));
}

// ---- against the eboot -------------------------------------------------------------------------

class PostEffectsGame : public EbootTest {
protected:
    const std::uint8_t* at(std::uint64_t link) {
        return reinterpret_cast<const std::uint8_t*>(bb::game::image().runtime(link));
    }
    // Whether the eboot holds `expected` at `link`.
    template <std::size_t N>
    bool holds(std::uint64_t link, const std::array<std::uint8_t, N>& expected) {
        return std::memcmp(at(link), expected.data(), N) == 0;
    }
    // The target of the rel32 call at `link`, as a link address.
    std::uint64_t call_target(std::uint64_t link) {
        EXPECT_EQ(at(link)[0], 0xe8) << std::hex << link;
        std::int32_t rel = 0;
        std::memcpy(&rel, at(link) + 1, sizeof rel);
        return link + 5 + static_cast<std::int64_t>(rel);
    }
};

TEST_F(PostEffectsGame, ThePrologueIsTheEboots) {
    EXPECT_TRUE(holds(bb::game::pfx_apply_link, bb::game::pfx_apply_prologue));
}

// rbx and r15 hold the block (pfx_apply's third argument) at these places: rbx is set from it
// at 0x021d6842 and r15 from rbx at 0x021d7e45. The tests check the reads and the calls; that
// register link is traced in docs/design/image-quality.md, not checked here.
TEST_F(PostEffectsGame, ChromaticAberrationReadsItsFields) {
    // cmpl $0, 0xac(%rbx); setne al; movzbl esi, al; mov rdi, r14; call SetChromaticAberrationEnable
    static_assert(bb::game::pfx_chromatic_aberration_enable * 4 == 0xac);
    EXPECT_TRUE(holds(0x021d7178, std::array<std::uint8_t, 7>{0x83, 0xbb, 0xac, 0, 0, 0, 0}));
    EXPECT_EQ(call_target(0x021d7188), 0x00bb37a0u);
    // vmovss xmm0, 0xb4(%rbx), the first dispersion; 0xb8 to 0xc0 follow and go to 0x00bb37d0.
    static_assert(bb::game::pfx_chromatic_aberration_first * 4 == 0xb4);
    static_assert(bb::game::pfx_chromatic_aberration_last * 4 == 0xc0);
    EXPECT_TRUE(holds(0x021d7193,
                      std::array<std::uint8_t, 8>{0xc5, 0xfa, 0x10, 0x83, 0xb4, 0, 0, 0}));
    EXPECT_TRUE(holds(0x021d71c6,
                      std::array<std::uint8_t, 8>{0xc5, 0xfa, 0x10, 0x83, 0xc0, 0, 0, 0}));
    EXPECT_EQ(call_target(0x021d71ea), 0x00bb37d0u);
}

TEST_F(PostEffectsGame, MotionBlurReadsItsFields) {
    // cmpl $0, 0x1dc(%rbx); ...; call the motion blur enable setter
    static_assert(bb::game::pfx_motion_blur_enable * 4 == 0x1dc);
    EXPECT_TRUE(holds(0x021d7d2e, std::array<std::uint8_t, 7>{0x83, 0xbb, 0xdc, 0x01, 0, 0, 0}));
    EXPECT_EQ(call_target(0x021d7d42), 0x00bb3870u);
    // vmovss xmm0, 0x1e4(%r15), the blur time ratio, the first argument of 0x00bb38f0
    static_assert(bb::game::pfx_motion_blur_time_ratio * 4 == 0x1e4);
    EXPECT_TRUE(holds(0x021d7f5f, std::array<std::uint8_t, 9>{0xc4, 0xc1, 0x7a, 0x10, 0x87, 0xe4,
                                                              0x01, 0, 0}));
    EXPECT_EQ(call_target(0x021d7f8b), 0x00bb38f0u);
}

TEST_F(PostEffectsGame, TheHookAppliesAndRemoves) {
    auto all = bb::hook::replacements();
    auto it = std::find_if(all.begin(), all.end(), [](const bb::hook::Replacement& r) {
        return r.link_addr == bb::game::pfx_apply_link;
    });
    ASSERT_NE(it, all.end());
    const std::array<bb::hook::Replacement, 1> hooked{*it};
    ASSERT_NO_THROW(bb::hook::apply(bb::game::image(), hooked));
    EXPECT_FALSE(holds(bb::game::pfx_apply_link, bb::game::pfx_apply_prologue));
    bb::hook::remove(bb::game::image(), hooked);
    EXPECT_TRUE(holds(bb::game::pfx_apply_link, bb::game::pfx_apply_prologue));
}

}  // namespace
