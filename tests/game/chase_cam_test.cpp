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
constexpr std::uint32_t value = 0x40, value_target = 0x44, written = 0x48, stick_turn = 0x4c;

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
    cam.set(value, 1.0f);
    cam.set(value_target, 5.0f);
    return cam;
}

// What the stand-in saw, and whether it resets the eased factors.
Camera seen;
bool reset_rates = false;

// A stand-in for the game's update with one filter of each kind: `value` follows its target by
// the self-turn factor +0x240, the stick turn adds the stick scale, `written` is set from a
// factor, and the eased factors move toward their targets by rate_ease a call.
void stand_in(void* cam_ptr, void*, void*, float) {
    auto& cam = *static_cast<Camera*>(cam_ptr);
    seen = cam;
    cam.set(value, cam.get(value) + (cam.get(value_target) - cam.get(value)) * cam.get(0x240));
    cam.set(stick_turn, cam.get(stick_turn) + cam.get(stick_x_scale));
    cam.set(0x330, 0.7f);  // a factor the call writes
    for (auto [rate, target, reset] : {std::array{follow_rate, follow_rate_target,
                                                  follow_rate_reset},
                                       std::array{lag_rate, lag_rate_target, lag_rate_reset}}) {
        cam.set(rate, reset_rates ? cam.get(reset)
                                  : cam.get(rate) + (cam.get(target) - cam.get(rate)) * rate_ease);
    }
}

TEST(ChaseCam, AtTheGamesRateOnlyCallsTheOriginal) {
    Camera direct = make_camera(), wrapped = make_camera();
    stand_in(&direct, nullptr, nullptr, 1.0f / 30);
    chase_cam_update_scaled(&stand_in, 1.0, &wrapped, nullptr, nullptr, 1.0f / 30);
    EXPECT_EQ(std::memcmp(seen.bytes.data(), make_camera().bytes.data(), cam_size), 0);
    EXPECT_EQ(std::memcmp(direct.bytes.data(), wrapped.bytes.data(), cam_size), 0);
}

TEST(ChaseCam, ScalesTheFactorsForOneCall) {
    const Camera own = make_camera();
    Camera cam = own;
    reset_rates = false;
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    for (std::uint32_t offset : factors) {
        const double k = own.get(offset);
        EXPECT_FLOAT_EQ(seen.get(offset), static_cast<float>(1 - std::sqrt(1 - k))) << offset;
    }
    EXPECT_FLOAT_EQ(seen.get(pitch_keep), std::sqrt(0.3f));
    EXPECT_FLOAT_EQ(seen.get(stick_x_scale), 0.04f);
    EXPECT_FLOAT_EQ(seen.get(stick_y_scale), 0.03f);

    // Afterwards the factors are their own again, except the one the call wrote.
    for (std::uint32_t offset : {0x1b8u, 0x1c4u, 0x23cu, 0x240u, 0x288u, 0x1a8u, 0x280u, 0x284u}) {
        EXPECT_EQ(cam.get(offset), own.get(offset)) << offset;
    }
    EXPECT_EQ(cam.get(0x330), 0.7f);
    // The eased factors moved from their own value by half a call's ease.
    const float half_ease = static_cast<float>(1 - std::sqrt(1 - double{rate_ease}));
    EXPECT_FLOAT_EQ(cam.get(follow_rate), 0.2f + (0.1f - 0.2f) * half_ease);
    EXPECT_FLOAT_EQ(cam.get(lag_rate), 0.15f + (0.1f - 0.15f) * half_ease);
}

TEST(ChaseCam, KeepsAResetOfTheEasedFactors) {
    Camera cam = make_camera();
    reset_rates = true;
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    reset_rates = false;
    EXPECT_EQ(cam.get(follow_rate), 0.25f);
    EXPECT_EQ(cam.get(lag_rate), 0.25f);
}

TEST(ChaseCam, TwoCallsAt60MatchOneAt30) {
    Camera at30 = make_camera(), at60 = make_camera();
    reset_rates = false;
    chase_cam_update_scaled(&stand_in, 1.0, &at30, nullptr, nullptr, 1.0f / 30);
    for (int i = 0; i < 2; ++i) {
        chase_cam_update_scaled(&stand_in, 2.0, &at60, nullptr, nullptr, 1.0f / 60);
    }
    for (std::uint32_t offset : {value, stick_turn, follow_rate, lag_rate}) {
        EXPECT_NEAR(at60.get(offset), at30.get(offset), 1e-6) << offset;
    }
}

TEST(ChaseCam, LeavesFactorsOutsideZeroToOne) {
    Camera cam = make_camera();
    cam.set(0x240, 1.5f);
    cam.set(pitch_keep, -0.5f);
    chase_cam_update_scaled(&stand_in, 2.0, &cam, nullptr, nullptr, 1.0f / 60);
    EXPECT_EQ(seen.get(0x240), 1.5f);
    EXPECT_EQ(seen.get(pitch_keep), -0.5f);
}

TEST(ChaseCam, IsRegisteredWithItsPrologue) {
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
