// Tests of the field-of-view option in src/game/camera/field_of_view.cpp (#666): the widener
// around a stand-in for the chase camera update that eases +0x50 as the game does, and, against
// the eboot, that the game's chase camera keeps the angle the option names at +0x50.
#include "game/camera/field_of_view.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/eboot.hpp"

namespace {

using bb::game::chase_cam_fov;
using bb::game::ChaseFovWidener;
using bb::game::game_fov;
using bb::test::EbootTest;

constexpr float deg = std::numbers::pi_v<float> / 180.0f;

struct Camera {
    alignas(16) std::array<std::byte, 0x340> bytes{};

    float fov() const {
        float v;
        std::memcpy(&v, bytes.data() + chase_cam_fov, sizeof v);
        return v;
    }
    void set_fov(float v) { std::memcpy(bytes.data() + chase_cam_fov, &v, sizeof v); }
};

// The game's ease of +0x50 (0x0143af5e to 0x0143af70): a quarter of the way to the row's angle.
float seen_fov = 0.0f;
void stand_in(Camera& cam, float target) {
    seen_fov = cam.fov();
    cam.set_fov(cam.fov() + (target - cam.fov()) * 0.25f);
}

// One update as the wrap runs it.
void update(ChaseFovWidener& w, Camera& cam, float target, float degrees) {
    w.restore(&cam);
    stand_in(cam, target);
    w.widen(&cam, degrees);
}

struct FovGuard {
    ~FovGuard() { bb::game::set_field_of_view(game_fov); }
};

TEST(FieldOfView, RefusesAnglesOutOfRange) {
    FovGuard guard;
    for (float bad : {29.9f, 70.1f, -43.0f, std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity()}) {
        EXPECT_THROW(bb::game::set_field_of_view(bad), std::invalid_argument) << bad;
    }
    EXPECT_EQ(bb::game::field_of_view(), game_fov);
    bb::game::set_field_of_view(63.0f);
    EXPECT_EQ(bb::game::field_of_view(), 63.0f);
    bb::game::set_field_of_view(bb::game::min_fov);
    bb::game::set_field_of_view(bb::game::max_fov);
}

TEST(FieldOfView, MovesAnAngleByTheDifference) {
    EXPECT_FLOAT_EQ(bb::game::widened_fov(43.0f * deg, 63.0f), 63.0f * deg);
    EXPECT_FLOAT_EQ(bb::game::widened_fov(38.0f * deg, 63.0f), 58.0f * deg);
    EXPECT_FLOAT_EQ(bb::game::widened_fov(48.0f * deg, 33.0f), 38.0f * deg);
    EXPECT_EQ(bb::game::widened_fov(0.7f, game_fov), 0.7f);
}

TEST(FieldOfView, TheGamesAngleLeavesTheFieldAlone) {
    ChaseFovWidener w;
    Camera cam;
    cam.set_fov(43.0f * deg);
    for (int i = 0; i < 5; ++i) {
        update(w, cam, 48.0f * deg, game_fov);
    }
    Camera plain;
    plain.set_fov(43.0f * deg);
    for (int i = 0; i < 5; ++i) {
        stand_in(plain, 48.0f * deg);
    }
    EXPECT_EQ(cam.fov(), plain.fov());
}

TEST(FieldOfView, TheUpdateSeesTheGamesAngleAndTheFieldTheWidenedOne) {
    ChaseFovWidener w;
    Camera cam, plain;
    cam.set_fov(43.0f * deg);
    plain.set_fov(43.0f * deg);
    // The row changes from 43 to 48 degrees and back; the game's ease must not see the option.
    for (int i = 0; i < 40; ++i) {
        const float target = (i < 20 ? 48.0f : 43.0f) * deg;
        stand_in(plain, target);
        const float game_seen = seen_fov;
        update(w, cam, target, 63.0f);
        EXPECT_EQ(seen_fov, game_seen) << i;
        EXPECT_FLOAT_EQ(cam.fov(), plain.fov() + 20.0f * deg) << i;
    }
}

TEST(FieldOfView, KeepsAValueWrittenBetweenUpdates) {
    ChaseFovWidener w;
    Camera cam;
    cam.set_fov(43.0f * deg);
    update(w, cam, 43.0f * deg, 63.0f);
    cam.set_fov(40.0f * deg);  // the game sets the field itself (a new parameter row)
    update(w, cam, 40.0f * deg, 63.0f);
    EXPECT_EQ(seen_fov, 40.0f * deg);
    EXPECT_FLOAT_EQ(cam.fov(), 60.0f * deg);
}

TEST(FieldOfView, ANewCameraStartsFromItsOwnAngle) {
    ChaseFovWidener w;
    Camera a, b;
    a.set_fov(43.0f * deg);
    update(w, a, 43.0f * deg, 63.0f);
    // A new chase camera (after a load) whose field happens to hold the widened bits.
    b.set_fov(a.fov());
    update(w, b, 43.0f * deg, 63.0f);
    EXPECT_FLOAT_EQ(seen_fov, 63.0f * deg);
}

TEST(FieldOfView, SwitchingTheOptionOffGivesTheGamesAngleBack) {
    ChaseFovWidener w;
    Camera cam;
    cam.set_fov(43.0f * deg);
    update(w, cam, 43.0f * deg, 63.0f);
    update(w, cam, 43.0f * deg, game_fov);
    EXPECT_FLOAT_EQ(cam.fov(), 43.0f * deg);
}

using FieldOfViewGame = EbootTest;

// chase_cam_construct stores the 43 degrees at +0x50 (movl $0x3f402037, 0x50(%rbx)) and the
// update eases +0x50 itself (vmovss 0x50(%r13), %xmm2 ... vmovss %xmm1, 0x50(%r13)).
TEST_F(FieldOfViewGame, TheChaseCameraKeepsItsAngleAtTheField) {
    const auto at = [](std::uint64_t link) {
        return reinterpret_cast<const std::uint8_t*>(bb::game::image().runtime(link));
    };
    constexpr std::array<std::uint8_t, 7> construct = {0xc7, 0x43, 0x50, 0x37, 0x20, 0x40, 0x3f};
    EXPECT_EQ(std::memcmp(at(0x0143a353), construct.data(), construct.size()), 0);
    float stored;
    std::memcpy(&stored, construct.data() + 3, sizeof stored);
    EXPECT_NEAR(stored / deg, game_fov, 0.001f);

    constexpr std::array<std::uint8_t, 6> load = {0xc4, 0xc1, 0x7a, 0x10, 0x55, 0x50};
    constexpr std::array<std::uint8_t, 6> store = {0xc4, 0xc1, 0x7a, 0x11, 0x4d, 0x50};
    EXPECT_EQ(std::memcmp(at(0x0143af5e), load.data(), load.size()), 0);
    EXPECT_EQ(std::memcmp(at(0x0143af70), store.data(), store.size()), 0);
}

}  // namespace
