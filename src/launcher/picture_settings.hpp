// The choices of the Picture and sound page and their mapping onto the config file
// (docs/design/launcher.md, "Picture and sound"). Pure, so tests/launcher covers it.
#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "common/config.hpp"

namespace bb::launcher {

enum class Sharpness { normal, sharpened, extra_sharp };

using Size = std::pair<unsigned, unsigned>;

struct PictureChoices {
    bool fullscreen = false;
    Size size{1280, 720};
    unsigned frame_rate = 30;  // 30, 60 or 120
    Sharpness sharpness = Sharpness::normal;
    bool operator==(const PictureChoices&) const = default;
};

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
}

// The common 16:9 sizes up to the display's size (always 1280 x 720), and `current` when it is
// not one of them, in order of area.
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
    if (std::find(out.begin(), out.end(), current) == out.end()) {
        out.push_back(current);
        std::sort(out.begin(), out.end(), [](const Size& a, const Size& b) {
            return std::pair(a.first * a.second, a.first) < std::pair(b.first * b.second, b.first);
        });
    }
    return out;
}

}  // namespace bb::launcher
