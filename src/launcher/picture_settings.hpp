// The choices of the Picture and sound page and their mapping onto the config file
// (docs/design/launcher.md, "Picture and sound"). Pure, so tests/launcher covers it.
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/config.hpp"

namespace bb::launcher {

// The sharpness slider: 0 is no FSR (the frame scaled bilinearly, bb-run's default), 1 FSR without
// RCAS, and 2 to 10 FSR with RCAS from 2 stops of attenuation down to 0 (the sharpest) in steps of
// a quarter stop. 9 is RCAS at bb-run's default attenuation.
inline constexpr int sharpness_off = 0;
inline constexpr int sharpness_no_rcas = 1;
inline constexpr int max_sharpness = 10;
inline constexpr float sharpness_stop = 0.25f;
inline constexpr float default_rcas_attenuation = 0.25f;  // bb-run's, with --fsr alone
inline constexpr float attenuation_of(int sharpness) {
    return static_cast<float>(max_sharpness - sharpness) * sharpness_stop;
}

// The post effect sliders, in percent of the game's strength: 0 switches the effect off.
inline constexpr int full_effect = 100;

using Size = std::pair<unsigned, unsigned>;

struct PictureChoices {
    bool fullscreen = false;
    Size size{1280, 720};
    unsigned frame_rate = 30;  // 30, 60, 120 or 144
    int sharpness = sharpness_off;  // 0 to max_sharpness, see above
    // The wide frame buffer (resolution, common::wide_resolutions) the game renders at; nothing
    // for bb-run's 16:9 frame. The page sets it from the window size or, in fullscreen, from the
    // display's shape (wide_frame).
    std::optional<Size> wide;
    unsigned anisotropy = 0;  // 0: the game's filtering, else common::valid_anisotropy
    float fov = common::game_fov;
    int motion_blur = full_effect;           // percent, 0 is off
    int chromatic_aberration = full_effect;  // percent, 0 is off
    common::PresentMode present_mode = common::PresentMode::mailbox;  // bb-run's default
    std::string device;   // a Vulkan device's name; empty for bb-run's choice
    std::string display;  // an SDL display's name; empty for the primary display
    bool operator==(const PictureChoices&) const = default;
};

// The texture filtering choices: the game's filtering (0), then the levels bb-run takes above
// none (#658). A level set by hand that is not here is added at the end of the list.
inline constexpr unsigned anisotropy_choices[] = {0, 2, 4, 8, 16};

// The percent a post effect's slider shows for its two keys.
inline int effect_percent(std::optional<bool> on, std::optional<float> strength) {
    if (!on.value_or(true)) {
        return 0;
    }
    return static_cast<int>(std::lround(strength.value_or(1.0f) * full_effect));
}

// Puts a moved post effect slider into its two keys: 0 % switches the effect off, 100 % is the
// game's strength (no strength key), anything between is that strength.
inline void apply_effect(int percent, std::optional<bool>& on, std::optional<float>& strength) {
    on = percent > 0;
    strength = percent > 0 && percent < full_effect
                   ? std::optional(static_cast<float>(percent) / full_effect)
                   : std::nullopt;
}

// What the page shows for `config`. Missing keys read as bb-run's defaults. A value between the
// slider's steps (an odd attenuation or strength) shows as the nearest step.
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
    c.motion_blur = effect_percent(config.motion_blur, config.motion_blur_strength);
    c.chromatic_aberration =
        effect_percent(config.chromatic_aberration, config.chromatic_aberration_strength);
    if (config.fsr.value_or(false)) {
        if (!config.rcas.value_or(true)) {
            c.sharpness = sharpness_no_rcas;
        } else {
            const float stops = config.rcas_attenuation.value_or(default_rcas_attenuation);
            const int level = max_sharpness - static_cast<int>(std::lround(stops / sharpness_stop));
            c.sharpness = std::clamp(level, sharpness_no_rcas + 1, max_sharpness);
        }
    }
    c.present_mode = config.present_mode.value_or(common::PresentMode::mailbox);
    c.device = config.device.value_or("");
    c.display = config.display.value_or("");
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
        // RCAS keys without fsr are an error in bb-run, so "off" removes them too.
        config.rcas.reset();
        config.rcas_attenuation.reset();
        config.fsr = after.sharpness != sharpness_off;
        if (after.sharpness == sharpness_no_rcas) {
            config.rcas = false;
        } else if (after.sharpness > sharpness_no_rcas &&
                   attenuation_of(after.sharpness) != default_rcas_attenuation) {
            config.rcas_attenuation = attenuation_of(after.sharpness);
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
        apply_effect(after.motion_blur, config.motion_blur, config.motion_blur_strength);
    }
    if (after.chromatic_aberration != before.chromatic_aberration) {
        apply_effect(after.chromatic_aberration, config.chromatic_aberration,
                     config.chromatic_aberration_strength);
    }
    if (after.present_mode != before.present_mode) {
        config.present_mode = after.present_mode == common::PresentMode::mailbox
                                  ? std::nullopt
                                  : std::optional(after.present_mode);
    }
    const auto name = [](const std::string& n) {
        return n.empty() ? std::nullopt : std::optional(n);
    };
    if (after.device != before.device) {
        config.device = name(after.device);
    }
    if (after.display != before.display) {
        config.display = name(after.display);
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
    for (const Size& s : sizes) {
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
    for (const Size& s : common) {
        if (s == common[0] || (s.first <= display.first && s.second <= display.second)) {
            out.push_back(s);
        }
    }
    for (const Size& s : wide_sizes(shape_of(display))) {
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
