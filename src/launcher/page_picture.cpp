// The Picture and sound page: the settings as one sentence with choices in it. Changes are
// written to the config file when the page closes.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "launcher/page.hpp"
#include "launcher/picture_settings.hpp"

namespace bb::launcher {

namespace {

// This page's own strings, in both languages. The sentence is cut into lines at its choices.
struct Text {
    const char* title;
    const char* lead;         // before the window / fullscreen choice
    const char* windowed;
    const char* fullscreen;
    const char* at_size;      // before the resolution
    const char* screen_size;  // fullscreen: in place of the resolution
    const char* rate_lead;
    const char* rate_tail;
    const char* look_lead;
    const char* normal;
    const char* sharpened;
    const char* extra_sharp;
    const char* look_tail;
    // The second sentence: view, textures, motion blur and colour fringes.
    const char* see_lead;
    const char* fov[3];         // fov_choices
    const char* fov_other;      // a value the list has no word for, in degrees
    const char* textures[3];    // anisotropy_choices
    const char* textures_other;  // another level, as a factor
    const char* blur[2];        // on, off
    const char* fringes[2];     // on, off
    const char* comma;
    const char* and_;
    const char* see_tail;
    const char* saved_note;
};
constexpr Text english{"Picture and sound", "I play in",       "a window",   "fullscreen",
                       "at",                "your screen's size", "at",       "frames per second,",
                       "with a",            "normal",          "sharpened",  "extra sharp",
                       "picture.",
                       "I see it with",
                       {"the game's view", "a wider view", "a much wider view"},
                       "a {}\xC2\xB0 view",
                       {"textures as sharp as in the game", "sharper distant textures",
                        "the sharpest distant textures"},
                       "{}\xC3\x97 filtered textures",
                       {"motion blur", "no motion blur"},
                       {"colour fringes at the edges", "no colour fringes"},
                       ",",
                       "and",
                       ".",
                       "Changes are saved when you go back."};
constexpr Text german{"Bild und Ton", "Ich spiele", "im Fenster",  "im Vollbild",
                      "in",           "der Größe deines Bildschirms", "mit", "Bildern pro Sekunde",
                      "und einem",    "normalen",   "geschärften", "extra scharfen",
                      "Bild.",
                      "Ich sehe es mit",
                      {"dem Blickfeld des Spiels", "einem weiteren Blickfeld",
                       "einem viel weiteren Blickfeld"},
                      "einem Blickfeld von {}\xC2\xB0",
                      {"Texturen so scharf wie im Spiel", "schärferen Texturen in der Ferne",
                       "den schärfsten Texturen in der Ferne"},
                      "{}-fach gefilterten Texturen",
                      {"Bewegungsunschärfe", "keiner Bewegungsunschärfe"},
                      {"Farbsäumen am Rand", "keinen Farbsäumen"},
                      ",",
                      "und",
                      ".",
                      "Änderungen werden beim Zurückgehen gespeichert."};

const Text& text(const Context& ctx) {
    return ctx.lang == Lang::de ? german : english;
}

Size display_size() {
    const SDL_DisplayID id = SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* m = id != 0 ? SDL_GetDesktopDisplayMode(id) : nullptr) {
        return {static_cast<unsigned>(std::max(m->w, 0)), static_cast<unsigned>(std::max(m->h, 0))};
    }
    return {0, 0};
}

std::string size_label(Size s) {
    return std::format("{} \xC3\x97 {}", s.first, s.second);  // "x" as the multiplication sign
}

// A combo of `labels` at the width of its widest label. Returns true when the choice changed.
bool choice(const char* id, const std::vector<std::string>& labels, int& index, float scale) {
    float width = 0;
    for (const std::string& l : labels) {
        width = std::max(width, ImGui::CalcTextSize(l.c_str()).x);
    }
    ImGui::SetNextItemWidth(width + (ImGui::GetFrameHeight() + 24 * scale));
    bool changed = false;
    if (ImGui::BeginCombo(id, labels[static_cast<size_t>(index)].c_str())) {
        for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
            const bool selected = i == index;
            if (ImGui::Selectable(labels[static_cast<size_t>(i)].c_str(), selected)) {
                changed = changed || i != index;
                index = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

class PicturePage : public Page {
public:
    const char* title(const Context& ctx) const override { return text(ctx).title; }

    void draw(const Context& ctx, float height) override {
        const bool first_frame = !loaded_;
        load(ctx);
        const Text& t = text(ctx);
        const float s = ctx.scale;
        const float gap = 8 * s;
        // Two sentences need more than the page's height at a small scale: the child scrolls,
        // and gamepad navigation scrolls it to the focused choice.
        ImGui::BeginChild("picture", ImVec2(432 * s, std::max(height - gap, 60 * s)));
        const PictureChoices was = now_;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.lead);
        ImGui::SameLine();
        if (first_frame) {  // gamepad navigation starts at the first choice
            ImGui::SetKeyboardFocusHere();
            ImGui::GetCurrentContext()->NavCursorVisible = true;
        }
        int mode = now_.fullscreen ? 1 : 0;
        if (choice("##mode", {t.windowed, t.fullscreen}, mode, s)) {
            now_.fullscreen = mode == 1;
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.at_size);
        ImGui::SameLine();
        if (now_.fullscreen) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(t.screen_size);
        } else {
            std::vector<std::string> labels;
            int index = 0;
            for (const Size sz : sizes_) {
                index = sz == now_.size ? static_cast<int>(labels.size()) : index;
                labels.push_back(size_label(sz));
            }
            if (choice("##size", labels, index, s)) {
                now_.size = sizes_[static_cast<size_t>(index)];
            }
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.rate_lead);
        ImGui::SameLine();
        int rate = 0;
        std::vector<std::string> rates;
        for (const unsigned r : common::frame_rates) {
            rate = r == now_.frame_rate ? static_cast<int>(rates.size()) : rate;
            rates.push_back(std::to_string(r));
        }
        if (choice("##rate", rates, rate, s)) {
            now_.frame_rate = common::frame_rates[static_cast<size_t>(rate)];
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.rate_tail);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.look_lead);
        ImGui::SameLine();
        int look = static_cast<int>(now_.sharpness);
        if (choice("##look", {t.normal, t.sharpened, t.extra_sharp}, look, s)) {
            now_.sharpness = static_cast<Sharpness>(look);
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.look_tail);

        ImGui::Dummy(ImVec2(0, gap / 2));
        draw_second_sentence(t, s);

        ImGui::Dummy(ImVec2(0, gap / 2));
        ImGui::TextDisabled("%s", t.saved_note);
        ImGui::EndChild();
        if (now_.fullscreen != was.fullscreen || now_.size != was.size) {
            now_.wide = wide_frame(now_, display_);
        }
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
    // "I see it with [view], [textures], [motion blur] and [colour fringes]."
    void draw_second_sentence(const Text& t, float s) {
        const auto tail = [s](const char* text) {
            ImGui::SameLine(0, 3 * s);  // clear of the focus frame
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.see_lead);
        ImGui::SameLine();

        std::vector<std::string> labels(std::begin(t.fov), std::end(t.fov));
        std::vector<float> fovs(std::begin(fov_choices), std::end(fov_choices));
        if (std::find(fovs.begin(), fovs.end(), now_.fov) == fovs.end()) {
            fovs.push_back(now_.fov);
            labels.push_back(std::vformat(t.fov_other, std::make_format_args(now_.fov)));
        }
        int index = static_cast<int>(std::find(fovs.begin(), fovs.end(), now_.fov) - fovs.begin());
        if (choice("##fov", labels, index, s)) {
            now_.fov = fovs[static_cast<size_t>(index)];
        }
        tail(t.comma);

        labels.assign(std::begin(t.textures), std::end(t.textures));
        std::vector<unsigned> levels(std::begin(anisotropy_choices), std::end(anisotropy_choices));
        if (std::find(levels.begin(), levels.end(), now_.anisotropy) == levels.end()) {
            levels.push_back(now_.anisotropy);
            labels.push_back(
                std::vformat(t.textures_other, std::make_format_args(now_.anisotropy)));
        }
        index = static_cast<int>(std::find(levels.begin(), levels.end(), now_.anisotropy) -
                                 levels.begin());
        if (choice("##textures", labels, index, s)) {
            now_.anisotropy = levels[static_cast<size_t>(index)];
        }
        tail(t.comma);

        int blur = now_.motion_blur ? 0 : 1;
        if (choice("##blur", {t.blur[0], t.blur[1]}, blur, s)) {
            now_.motion_blur = blur == 0;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(t.and_);

        int fringes = now_.chromatic_aberration ? 0 : 1;
        if (choice("##fringes", {t.fringes[0], t.fringes[1]}, fringes, s)) {
            now_.chromatic_aberration = fringes == 0;
        }
        tail(t.see_tail);
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
        display_ = display_size();
        sizes_ = resolution_choices(display_, now_.size);
    }

    bool loaded_ = false;
    PictureChoices before_;
    PictureChoices now_;
    Size display_{0, 0};
    std::vector<Size> sizes_;
};

}  // namespace

std::unique_ptr<Page> make_picture_page() {
    return std::make_unique<PicturePage>();
}

}  // namespace bb::launcher
