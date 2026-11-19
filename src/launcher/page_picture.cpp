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
    const char* saved_note;
};
constexpr Text english{"Picture and sound", "I play in",       "a window",   "fullscreen",
                       "at",                "your screen's size", "at",       "frames per second,",
                       "with a",            "normal",          "sharpened",  "extra sharp",
                       "picture.",          "Changes are saved when you go back."};
constexpr Text german{"Bild und Ton", "Ich spiele", "im Fenster",  "im Vollbild",
                      "in",           "der Größe deines Bildschirms", "mit", "Bildern pro Sekunde",
                      "und einem",    "normalen",   "geschärften", "extra scharfen",
                      "Bild.",        "Änderungen werden beim Zurückgehen gespeichert."};

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

    void draw(const Context& ctx, float) override {
        const bool first_frame = !loaded_;
        load(ctx);
        const Text& t = text(ctx);
        const float s = ctx.scale;
        const float gap = 8 * s;
        ImGui::Indent(24 * s);
        ImGui::Dummy(ImVec2(0, gap));

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

        ImGui::Dummy(ImVec2(0, gap));
        ImGui::TextDisabled("%s", t.saved_note);
        ImGui::Unindent(24 * s);
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
        sizes_ = resolution_choices(display_size(), now_.size);
    }

    bool loaded_ = false;
    PictureChoices before_;
    PictureChoices now_;
    std::vector<Size> sizes_;
};

}  // namespace

std::unique_ptr<Page> make_picture_page() {
    return std::make_unique<PicturePage>();
}

}  // namespace bb::launcher
