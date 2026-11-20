// The choices of the Picture and sound page and their mapping onto the config file
// (docs/design/launcher.md, "Picture and sound"). Pure, so tests/launcher covers it.
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "common/config.hpp"

namespace bb::launcher {

enum class Sharpness { normal, sharpened, extra_sharp };

using Size = std::pair<unsigned, unsigned>;

struct PictureChoices {
    bool fullscreen = false;
    Size size{1280, 720};
    unsigned frame_rate = 30;  // 30, 60, 120 or 144
    Sharpness sharpness = Sharpness::normal;
    // The wide frame buffer (resolution, common::wide_resolutions) the game renders at; nothing
    // for bb-run's 16:9 frame. The page sets it from the window size or, in fullscreen, from the
    // display's shape (wide_frame).
    std::optional<Size> wide;
    unsigned anisotropy = 0;  // 0: the game's filtering, else common::valid_anisotropy
    float fov = common::game_fov;
    bool motion_blur = true;
    bool chromatic_aberration = true;
    bool operator==(const PictureChoices&) const = default;
};

// The field-of-view choices: the game's angle, a wider and a much wider one (#666).
inline constexpr float fov_choices[] = {common::game_fov, 52.0f, 60.0f};
// The anisotropy choices: the game's filtering, sharper and the sharpest (#658).
inline constexpr unsigned anisotropy_choices[] = {0, 4, 16};

// What the page shows for `config`. Missing keys read as bb-run's defaults. An FSR setting that
// no choice stands for (RCAS off, an odd attenuation) shows as the nearest choice.
inline PictureChoices choices_from_config(const common::Config& config) {
    PictureChoices c;
    c.fullscreen = config.fullscreen.value_or(false);
    if (config.window_size) {
        c.size = *config.window_size;
    }
    const unsigned rate = config.frame_rate.value_or(30);
    c.frame_rate = common::valid_frame_rate(rate) ? rate : 30;
    if (config.resolution &&
        common::is_wide_resolution(config.resolution->first, config.resolution->second)) {
        c.wide = config.resolution;
    }
    c.anisotropy = config.anisotropy.value_or(0);
    c.fov = config.fov.value_or(common::game_fov);
    c.motion_blur = config.motion_blur.value_or(true);
    c.chromatic_aberration = config.chromatic_aberration.value_or(true);
    if (config.fsr.value_or(false) && config.rcas.value_or(true)) {
        c.sharpness = config.rcas_attenuation && *config.rcas_attenuation == 0.0f
                          ? Sharpness::extra_sharp
                          : Sharpness::sharpened;
    }
    return c;
}

// Puts the choices that differ between `before` (what the page showed on opening) and `after`
// into `config`. Everything the player left alone stays as the file had it, so a setting the page
// has no choice for (an odd window size, an RCAS attenuation of 0.5) survives.
inline void apply_choices(common::Config& config, const PictureChoices& before,
                          const PictureChoices& after) {
    if (after.fullscreen != before.fullscreen) {
        config.fullscreen = after.fullscreen;
    }
    if (after.size != before.size) {
        config.window_size = after.size;
    }
    if (after.frame_rate != before.frame_rate) {
        config.frame_rate = after.frame_rate;
    }
    if (after.sharpness != before.sharpness) {
        // RCAS keys without fsr are an error in bb-run, so "normal" removes them too.
        config.rcas.reset();
        config.rcas_attenuation.reset();
        config.fsr = after.sharpness != Sharpness::normal;
        if (after.sharpness == Sharpness::extra_sharp) {
            config.rcas_attenuation = 0.0f;
        }
    }
    if (after.wide != before.wide) {
        config.resolution = after.wide;  // none: bb-run's 16:9 frame
    }
    if (after.anisotropy != before.anisotropy) {
        config.anisotropy = after.anisotropy == 0 ? std::nullopt : std::optional(after.anisotropy);
    }
    if (after.fov != before.fov) {
        config.fov = after.fov == common::game_fov ? std::nullopt : std::optional(after.fov);
    }
    if (after.motion_blur != before.motion_blur) {
        config.motion_blur = after.motion_blur;
    }
    if (after.chromatic_aberration != before.chromatic_aberration) {
        config.chromatic_aberration = after.chromatic_aberration;
    }
}

// The shape of a display: 16:9 (and anything narrower than 2:1), 21:9 (2:1 to 3:1) or 32:9.
enum class Shape { normal, wide, super_wide };
inline Shape shape_of(Size s) {
    if (s.second == 0 || s.first < 2 * s.second) {
        return Shape::normal;
    }
    return s.first < 3 * s.second ? Shape::wide : Shape::super_wide;
}

// The wide frame buffer sizes of `shape`, in order of area.
inline std::vector<Size> wide_sizes(Shape shape) {
    std::vector<Size> out;
    for (const common::WideResolution r : common::wide_resolutions) {
        if (shape != Shape::normal && shape_of({r.width, r.height}) == shape) {
            out.push_back({r.width, r.height});
        }
    }
    return out;
}

// The wide frame the game renders at for `c` on `display`. A window at a wide size renders at
// that size. Fullscreen on a 21:9 or 32:9 display renders at the smallest wide size of that shape
// that is at least as tall as the display (else the largest), so the frame fills the screen and
// FSR only ever scales it up. Anything else renders bb-run's 16:9 frame.
inline std::optional<Size> wide_frame(const PictureChoices& c, Size display) {
    if (!c.fullscreen) {
        return common::is_wide_resolution(c.size.first, c.size.second) ? std::optional(c.size)
                                                                         : std::nullopt;
    }
    const std::vector<Size> sizes = wide_sizes(shape_of(display));
    if (sizes.empty()) {
        return std::nullopt;
    }
    for (const Size s : sizes) {
        if (s.second >= display.second) {
            return s;
        }
    }
    return sizes.back();
}

// The common 16:9 sizes up to the display's size (always 1280 x 720), on a 21:9 or 32:9 display
// the wide sizes of its shape that fit it, and `current` when it is not one of them, in order of
// area.
inline std::vector<Size> resolution_choices(Size display, Size current) {
    static constexpr Size common[] = {{1280, 720},  {1600, 900},  {1920, 1080},
                                      {2560, 1440}, {3200, 1800}, {3840, 2160},
                                      {5120, 2880}, {7680, 4320}};
    std::vector<Size> out;
    for (const Size s : common) {
        if (s == common[0] || (s.first <= display.first && s.second <= display.second)) {
            out.push_back(s);
        }
    }
    for (const Size s : wide_sizes(shape_of(display))) {
        if (s.first <= display.first && s.second <= display.second) {
            out.push_back(s);
        }
    }
    if (std::find(out.begin(), out.end(), current) == out.end()) {
        out.push_back(current);
    }
    std::sort(out.begin(), out.end(), [](const Size& a, const Size& b) {
        return std::pair(a.first * a.second, a.first) < std::pair(b.first * b.second, b.first);
    });
    return out;
}

}  // namespace bb::launcher
