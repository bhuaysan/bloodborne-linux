// The Picture and sound page: a settings form, one row per setting with its name on the left and
// its control on the right (docs/design/launcher.md, "Picture and sound"). Changes are written to
// the config file when the page closes.
#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "launcher/form.hpp"
#include "launcher/gpu_probe.hpp"
#include "launcher/page.hpp"
#include "launcher/picture_settings.hpp"
#include "launcher/widgets.hpp"

namespace bb::launcher {

namespace {

// This page's own strings, in both languages.
struct Text {
    const char* title;
    const char* mode;
    const char* modes[2];  // window, fullscreen
    const char* display;
    const char* main_display;  // no display key: the primary display
    const char* resolution;
    const char* screen_size;  // fullscreen: in place of the resolution
    const char* frame_rate;
    const char* sync;
    const char* syncs[3];  // sync_modes
    const char* gpu;
    const char* automatic;  // no device key: bb-run's choice
    const char* sharpness;
    const char* off;  // a slider at 0
    const char* fov;
    const char* fov_default;  // the game's angle, in degrees
    const char* textures;
    const char* game_textures;  // anisotropy 0: the game's own filtering
    const char* motion_blur;
    const char* fringes;
};
constexpr Text english{
    .title = "Picture and sound",
    .mode = "Display mode",
    .modes = {"Window", "Fullscreen"},
    .display = "Monitor",
    .main_display = "Main display",
    .resolution = "Resolution",
    .screen_size = "Screen size",
    .frame_rate = "Frame rate",
    .sync = "VSync",
    .syncs = {"Triple buffering", "On", "Off"},
    .gpu = "Graphics card",
    .automatic = "Automatic",
    .sharpness = "Sharpness",
    .off = "Off",
    .fov = "Field of view",
    .fov_default = "{}\xC2\xB0 (default)",
    .textures = "Texture filtering",
    .game_textures = "Default",
    .motion_blur = "Motion blur",
    .fringes = "Colour fringes",
};
constexpr Text german{
    .title = "Bild und Ton",
    .mode = "Anzeigemodus",
    .modes = {"Fenster", "Vollbild"},
    .display = "Bildschirm",
    .main_display = "Hauptbildschirm",
    .resolution = "Aufl\xc3\xb6sung",
    .screen_size = "Bildschirmgr\xc3\xb6\xc3\x9f" "e",
    .frame_rate = "Bildrate",
    .sync = "VSync",
    .syncs = {"Dreifachpuffer", "An", "Aus"},
    .gpu = "Grafikkarte",
    .automatic = "Automatisch",
    .sharpness = "Bildsch\xc3\xa4rfe",
    .off = "Aus",
    .fov = "Sichtfeld",
    .fov_default = "{}\xC2\xB0 (Standard)",
    .textures = "Texturfilterung",
    .game_textures = "Standard",
    .motion_blur = "Bewegungsunsch\xc3\xa4rfe",
    .fringes = "Farbs\xc3\xa4ume",
};

const Text& text(Lang lang) {
    return lang == Lang::de ? german : english;
}

// The VSync row's entries, in the order of Text::syncs: mailbox (bb-run's default) first.
constexpr common::PresentMode sync_modes[] = {
    common::PresentMode::mailbox, common::PresentMode::fifo, common::PresentMode::immediate};

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    const auto lower = [](char c) { return std::tolower(static_cast<unsigned char>(c)); };
    return std::search(text.begin(), text.end(), part.begin(), part.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}

// The names of the displays SDL lists, in its order.
std::vector<std::string> display_names() {
    std::vector<std::string> names;
    int count = 0;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
        for (int i = 0; i < count; ++i) {
            const char* name = SDL_GetDisplayName(ids[i]);
            if (name != nullptr && *name != '\0') {
                names.emplace_back(name);
            }
        }
        SDL_free(ids);
    }
    return names;
}

// The size of the display bb-run opens on for `name` (as bb-run --display matches it), the
// primary display for an empty name or one that matches none.
Size display_size(const std::string& name) {
    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    int count = 0;
    if (SDL_DisplayID* ids = name.empty() ? nullptr : SDL_GetDisplays(&count)) {
        for (int i = 0; i < count; ++i) {
            const char* n = SDL_GetDisplayName(ids[i]);
            if (n != nullptr && contains_ignoring_case(n, name)) {
                id = ids[i];
                break;
            }
        }
        SDL_free(ids);
    }
    if (const SDL_DisplayMode* m = id != 0 ? SDL_GetDesktopDisplayMode(id) : nullptr) {
        return {static_cast<unsigned>(std::max(m->w, 0)), static_cast<unsigned>(std::max(m->h, 0))};
    }
    return {0, 0};
}

std::string size_label(Size s) {
    return std::format("{} \xC3\x97 {}", s.first, s.second);  // "x" as the multiplication sign
}

class PicturePage : public Page {
public:
    const char* title(const Context& ctx) const override { return text(ctx.lang).title; }

    Action draw(const Context& ctx, float height) override {
        const bool first_frame = !loaded_;
        load(ctx);
        const Text& t = text(ctx.lang);
        const float s = ctx.scale;
        const PictureChoices was = now_;
        draw_form(t, s, height, first_frame);
        if (now_.display != was.display) {
            display_ = display_size(now_.display);
            sizes_ = resolution_choices(display_, now_.size);
        }
        if (now_.fullscreen != was.fullscreen || now_.size != was.size ||
            now_.display != was.display) {
            now_.wide = wide_frame(now_, display_);
        }
        return Action::none;
    }

    void on_close(const Context& ctx) override {
        if (!loaded_ || now_ == before_ || ctx.config_path.empty()) {
            return;
        }
        // Read again: the file is the only state, and other keys (game folder, profile) stay.
        try {
            common::update_config(ctx.config_path, [&](common::Config& config) {
                apply_choices(config, before_, now_);
            });
        } catch (const common::ConfigError&) {
            // An unreadable file is left as it is; the player sees the old settings next time.
        }
    }

private:
    void draw_form(const Text& t, float s, float height, bool first_frame) {
        Form form("picture", height, s, first_frame);

        int mode = now_.fullscreen ? 1 : 0;
        if (form.row(t.mode,
                     [&] { return dropdown("##mode", {t.modes[0], t.modes[1]}, mode); })) {
            now_.fullscreen = mode == 1;
        }
        form.row(t.display,
            [&] { return name_dropdown("##display", t.main_display, displays_, now_.display); });
        if (now_.fullscreen) {
            form.row(t.resolution, [&] {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", t.screen_size);
                return false;
            });
        } else {
            std::vector<std::string> labels;
            int index = 0;
            for (const Size& sz : sizes_) {
                index = sz == now_.size ? static_cast<int>(labels.size()) : index;
                labels.push_back(size_label(sz));
            }
            if (form.row(t.resolution, [&] { return dropdown("##size", labels, index); })) {
                now_.size = sizes_[static_cast<size_t>(index)];
            }
        }

        std::vector<std::string> rates;
        int rate = 0;
        for (const unsigned r : common::frame_rates) {
            rate = r == now_.frame_rate ? static_cast<int>(rates.size()) : rate;
            rates.push_back(std::format("{} FPS", r));
        }
        if (form.row(t.frame_rate, [&] { return dropdown("##rate", rates, rate); })) {
            now_.frame_rate = common::frame_rates[static_cast<size_t>(rate)];
        }

        int sync = static_cast<int>(std::find(std::begin(sync_modes), std::end(sync_modes),
                                              now_.present_mode) -
                                    std::begin(sync_modes));
        if (form.row(t.sync,
                [&] { return dropdown("##sync", {t.syncs[0], t.syncs[1], t.syncs[2]}, sync); })) {
            now_.present_mode = sync_modes[sync];
        }
        form.row(t.gpu, [&] { return name_dropdown("##gpu", t.automatic, gpus_, now_.device); });

        // Whole degrees; a fractional angle in the file stays until the slider moves.
        int degrees = static_cast<int>(std::lround(now_.fov));
        const int game = static_cast<int>(common::game_fov);
        const auto angle = [&](int d) {
            return d == game ? std::vformat(t.fov_default, std::make_format_args(d))
                             : std::format("{}\xC2\xB0", d);
        };
        const auto percent = [&](int p) {
            return p == 0 ? std::string(t.off) : std::format("{} %", p);
        };
        // One value column for every slider, as wide as the widest value ("43° (default)"), so
        // the lines have the same length.
        const float value_w =
            value_column({angle(game), percent(full_effect), std::string(t.off)}, s);

        form.row(t.sharpness, [&] {
            return line_slider("##sharp", now_.sharpness, sharpness_off, max_sharpness,
                               sharpness_off, 1, value_w, [&](int v) {
                                   return v == sharpness_off ? std::string(t.off)
                                                             : std::to_string(v);
                               }, s);
        });

        if (form.row(t.fov, [&] {
                return line_slider("##fov", degrees, static_cast<int>(common::min_fov),
                                   static_cast<int>(common::max_fov), game, 1, value_w, angle,
                                   s);
            })) {
            now_.fov = static_cast<float>(degrees);
        }

        std::vector<unsigned> levels(std::begin(anisotropy_choices), std::end(anisotropy_choices));
        if (std::find(levels.begin(), levels.end(), now_.anisotropy) == levels.end()) {
            levels.push_back(now_.anisotropy);  // a level set by hand goes at the end
        }
        std::vector<std::string> filters;
        int level = 0;
        for (const unsigned l : levels) {
            level = l == now_.anisotropy ? static_cast<int>(filters.size()) : level;
            filters.push_back(l == 0 ? t.game_textures : std::format("{}\xC3\x97", l));
        }
        if (form.row(t.textures, [&] { return dropdown("##textures", filters, level); })) {
            now_.anisotropy = levels[static_cast<size_t>(level)];
        }

        form.row(t.motion_blur, [&] {
            return line_slider("##blur", now_.motion_blur, 0, full_effect, full_effect, 5,
                               value_w, percent, s);
        });
        form.row(t.fringes, [&] {
            return line_slider("##fringes", now_.chromatic_aberration, 0, full_effect,
                               full_effect, 5, value_w, percent, s);
        });

    }

    void load(const Context& ctx) {
        if (loaded_) {
            return;
        }
        loaded_ = true;
        common::Config config;
        try {
            if (!ctx.config_path.empty() && std::filesystem::exists(ctx.config_path)) {
                config = common::read_config(ctx.config_path).config;
            }
        } catch (const common::ConfigError&) {
            config = {};
        }
        before_ = now_ = choices_from_config(config);
        displays_ = display_names();
        gpus_ = vulkan_gpu_names();
        display_ = display_size(now_.display);
        sizes_ = resolution_choices(display_, now_.size);
    }

    bool loaded_ = false;
    PictureChoices before_;
    PictureChoices now_;
    Size display_{0, 0};
    std::vector<Size> sizes_;
    std::vector<std::string> displays_;
    std::vector<std::string> gpus_;
};

}  // namespace

std::unique_ptr<Page> make_picture_page() {
    return std::make_unique<PicturePage>();
}

std::vector<const char*> picture_strings(Lang lang) {
    const Text& t = text(lang);
    return {t.title,       t.mode,       t.modes[0],    t.modes[1],       t.display,
            t.main_display, t.resolution, t.screen_size, t.frame_rate,    t.sync,
            t.syncs[0],    t.syncs[1],   t.syncs[2],    t.gpu,            t.automatic,
            t.sharpness,   t.off,        t.fov,         t.fov_default,    t.textures,
            t.game_textures, t.motion_blur, t.fringes};
}

}  // namespace bb::launcher
