// Tests of the chase camera wrapper in src/game/camera/: the factor scaling around a stand-in for
// the game's update, and the hook's prologue against the eboot.
#include "game/camera/chase_cam.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/eboot.hpp"
#include "hook/hook.hpp"

namespace {

using namespace bb::game::chase_cam;
using bb::game::chase_cam_update_scaled;
using bb::test::EbootTest;

constexpr std::size_t cam_size = 0x340;
// Fields of the stand-in's own: values that follow a target, and what it used.
constexpr std::uint32_t value = 0x40, value_target = 0x44, stick_turn = 0x4c, follow_value = 0x50,
                        lag_value = 0x54, lag_used = 0x58, weight_used = 0x5c;

struct Camera {
    alignas(16) std::array<std::byte, cam_size> bytes{};

    float get(std::uint32_t offset) const {
        float v;
        std::memcpy(&v, bytes.data() + offset, sizeof v);
        return v;
    }
    void set(std::uint32_t offset, float v) { std::memcpy(bytes.data() + offset, &v, sizeof v); }
};

// Every field the wrapper scales, with a value of its own.
Camera make_camera() {
    Camera cam;
    float k = 0.05f;
    for (std::uint32_t offset : factors) {
        cam.set(offset, k);
        k += 0.05f;
    }
    cam.set(follow_rate_target, 0.1f);
    cam.set(follow_rate_reset, 0.25f);
    cam.set(follow_rate, 0.2f);
    cam.set(lag_rate_target, 0.1f);
    cam.set(lag_rate_reset, 0.25f);
    cam.set(lag_rate, 0.15f);
    cam.set(pitch_keep, 0.3f);
    cam.set(stick_x_scale, 0.08f);
    cam.set(stick_y_scale, 0.06f);
    cam.set(input_weight, 0.6f);
    cam.set(input_hold, 0.0f);
    cam.set(input_fade_time, 0.5f);
    cam.set(value, 1.0f);
    cam.set(value_target, 5.0f);
    return cam;
}

// What the stand-in saw, and which path it takes.
Camera seen;
enum class Path { ease, reset, skip } path = Path::ease;
bool camera_input = false;

// A stand-in for the game's update with one filter of each kind, in the game's order: `value`
// follows its target by the self-turn factor +0x240, follow_value by follow_rate before the
// ease; the stick turn adds the stick scale; the input weight is held, dropped or set to 1;
// the eased factors are eased, reset or left alone; lag_value follows by lag_rate pulled toward
// 1 by the weight, after the ease.
void stand_in(void* cam_ptr, void*, void*, float dt) {
    auto& cam = *static_cast<Camera*>(cam_ptr);
    seen = cam;
    auto follow = [&](std::uint32_t at, float k) {
        cam.set(at, cam.get(at) + (cam.get(value_target) - cam.get(at)) * k);
    };
    follow(value, cam.get(0x240));
    follow(follow_value, cam.get(follow_rate));
    cam.set(stick_turn, cam.get(stick_turn) + cam.get(stick_x_scale));

    cam.set(input_hold, std::max(0.0f, cam.get(input_hold) - dt));
    if (!(0.0f < cam.get(input_hold))) {
        cam.set(input_weight, cam.get(input_weight) - dt / cam.get(input_fade_time));
        if (0.0f > cam.get(input_weight)) {
            cam.set(input_weight, 0.0f);
        }
    }
    if (camera_input) {
        cam.set(input_weight, 1.0f);
        cam.set(input_hold, 0.2f);
    }

    for (auto [rate, target, reset] : {std::array{follow_rate, follow_rate_target,
                                                  follow_rate_reset},
                                       std::array{lag_rate, lag_rate_target, lag_rate_reset}}) {
        if (path == Path::reset) {
            cam.set(rate, cam.get(reset));
        } else if (path == Path::ease) {
            cam.set(rate, cam.get(rate) + (cam.get(target) - cam.get(rate)) * rate_ease);
        }
    }
    const float w = cam.get(input_weight);
    cam.set(weight_used, w);
    cam.set(lag_used, cam.get(lag_rate));
    follow(lag_value, cam.get(lag_rate) + (1 - cam.get(lag_rate)) * w);
}

// 1 - (1 - k)^(1/2), the factor per half call.
float half(float k) {
    return static_cast<float>(1 - std::sqrt(1 - double{k}));
}

class ChaseCam : public ::testing::Test {
protected:
    void SetUp() override {
        path = Path::ease;
        camera_input = false;
    }
};

TEST_F(ChaseCam, AtTheGamesRateOnlyCallsTheOriginal) {
    Camera direct = make_camera(), wrapped = make_camera();
    stand_in(&direct, nullptr, nullptr, 1.0f / 30);
    chase_cam_update_scaled(&stand_in, 1.0, &wrapped, nullptr, nullptr, 1.0f / 30);
    EXPECT_EQ(std::memcmp(seen.bytes.data(), make_camera().bytes.data(), cam_size), 0);
    EXPECT_EQ(std::memcmp(direct.bytes.data(), wrapped.bytes.data(), cam_size), 0);
}

TEST_F(ChaseCam, ScalesTheFactorsForOneCall) {
    const Camera own = make_camera();
    Camera cam = own;
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    for (std::uint32_t offset : factors) {
        if (offset != lag_rate) {
            EXPECT_FLOAT_EQ(seen.get(offset), half(own.get(offset))) << offset;
        }
    }
    EXPECT_FLOAT_EQ(seen.get(pitch_keep), std::sqrt(0.3f));
    EXPECT_FLOAT_EQ(seen.get(stick_x_scale), 0.04f);
    EXPECT_FLOAT_EQ(seen.get(stick_y_scale), 0.03f);
    // lag_rate is used after its ease: the scaled value of its value eased by half a call.
    const float lag_half = 0.15f + (0.1f - 0.15f) * half(rate_ease);
    const float follow_half = 0.2f + (0.1f - 0.2f) * half(rate_ease);
    EXPECT_FLOAT_EQ(cam.get(lag_used), half(lag_half));
    // The weight is used dropped by the step, then scaled.
    EXPECT_FLOAT_EQ(cam.get(weight_used), half(0.6f - (1.0f / 60) / 0.5f));

    // Afterwards the plain factors are their own again, the eased ones eased by half a call,
    // the weight dropped by the step.
    for (std::uint32_t offset : {0x1b8u, 0x1c4u, 0x1c8u, 0x1ccu, 0x23cu, 0x240u, 0x288u, 0x330u,
                                 0x1a8u, 0x280u, 0x284u}) {
        EXPECT_EQ(cam.get(offset), own.get(offset)) << offset;
    }
    EXPECT_FLOAT_EQ(cam.get(follow_rate), follow_half);
    EXPECT_FLOAT_EQ(cam.get(lag_rate), lag_half);
    EXPECT_FLOAT_EQ(cam.get(input_weight), 0.6f - (1.0f / 60) / 0.5f);
}

TEST_F(ChaseCam, KeepsAResetOfTheEasedFactors) {
    Camera cam = make_camera();
    path = Path::reset;
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    EXPECT_EQ(cam.get(follow_rate), 0.25f);
    EXPECT_EQ(cam.get(lag_rate), 0.25f);
}

TEST_F(ChaseCam, LeavesEasedFactorsThatTheCallLeftAlone) {
    Camera cam = make_camera();
    path = Path::skip;
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    EXPECT_EQ(cam.get(follow_rate), 0.2f);
    EXPECT_EQ(cam.get(lag_rate), 0.15f);
}

TEST_F(ChaseCam, KeepsTheWeightOfCameraInput) {
    Camera cam = make_camera();
    camera_input = true;
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    EXPECT_EQ(cam.get(input_weight), 1.0f);
    EXPECT_EQ(cam.get(weight_used), 1.0f);
}

TEST_F(ChaseCam, ScalesAHeldWeightWithoutDroppingIt) {
    Camera cam = make_camera();
    cam.set(input_hold, 0.1f);
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    EXPECT_FLOAT_EQ(cam.get(weight_used), half(0.6f));
    EXPECT_EQ(cam.get(input_weight), 0.6f);
}

TEST_F(ChaseCam, TwoCallsAt60MatchOneAt30) {
    Camera at30 = make_camera(), at60 = make_camera(), unscaled = make_camera();
    chase_cam_update_scaled(&stand_in, 1.0, &at30, nullptr, nullptr, 1.0f / 30);
    for (int i = 0; i < 2; ++i) {
        chase_cam_update_scaled(&stand_in, 2.0, &at60, nullptr, nullptr, 1.0f / 60);
        stand_in(&unscaled, nullptr, nullptr, 1.0f / 60);
    }
    // Exact for filters with a constant factor, the eases and the weight.
    for (std::uint32_t offset : {value, stick_turn, follow_rate, lag_rate, input_weight}) {
        EXPECT_NEAR(at60.get(offset), at30.get(offset), 1e-6) << offset;
    }
    // follow_value and lag_value: their factors change between the two half calls (the ease,
    // the weight's drop), which one call at 30 doesn't see. Within that change, and far closer
    // than the original's two calls at 60.
    for (std::uint32_t offset : {follow_value, lag_value}) {
        const float error = std::abs(at60.get(offset) - at30.get(offset));
        EXPECT_LT(error, 0.1f) << offset;
        EXPECT_LT(error * 5, std::abs(unscaled.get(offset) - at30.get(offset))) << offset;
    }
}

TEST_F(ChaseCam, LeavesFactorsOutsideZeroToOne) {
    Camera cam = make_camera();
    cam.set(0x240, 1.5f);
    cam.set(pitch_keep, -0.5f);
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    EXPECT_EQ(seen.get(0x240), 1.5f);
    EXPECT_EQ(seen.get(pitch_keep), -0.5f);
}

TEST_F(ChaseCam, IsRegisteredWithItsPrologue) {
    auto all = bb::hook::replacements();
    auto it = std::find_if(all.begin(), all.end(), [](const bb::hook::Replacement& r) {
        return std::string_view(r.name) == "bb::game::chase_cam_update";
    });
    ASSERT_NE(it, all.end());
    EXPECT_EQ(it->link_addr, 0x0143ac60U);
    ASSERT_EQ(it->prologue.size(), bb::game::chase_cam_prologue.size());
    EXPECT_TRUE(std::equal(it->prologue.begin(), it->prologue.end(),
                           bb::game::chase_cam_prologue.begin()));
}

using ChaseCamGame = EbootTest;

TEST_F(ChaseCamGame, ThePrologueIsTheEboots) {
    const auto& prologue = bb::game::chase_cam_prologue;
    EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(bb::game::image().runtime(0x0143ac60)),
                          prologue.data(), prologue.size()),
              0);
}

}  // namespace
