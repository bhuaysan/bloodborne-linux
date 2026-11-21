// The Picture and sound page: a settings form, one row per setting with its name on the left and
// its control on the right (docs/design/launcher.md, "Picture and sound"). Changes are written to
// the config file when the page closes.
#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "common/config.hpp"
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
    const char* resolution;
    const char* screen_size;  // fullscreen: in place of the resolution
    const char* frame_rate;
    const char* sharpness;
    const char* sharpnesses[3];  // Sharpness
    const char* fov;
    const char* textures;
    const char* game_textures;  // anisotropy 0
    const char* motion_blur;
    const char* fringes;
};
constexpr Text english{
    .title = "Picture and sound",
    .mode = "Display mode",
    .modes = {"Window", "Fullscreen"},
    .resolution = "Resolution",
    .screen_size = "Screen size",
    .frame_rate = "Frame rate",
    .sharpness = "Sharpness",
    .sharpnesses = {"Normal", "Sharpened", "Extra sharp"},
    .fov = "Field of view",
    .textures = "Texture filtering",
    .game_textures = "As in the game",
    .motion_blur = "Motion blur",
    .fringes = "Colour fringes",
};
constexpr Text german{
    .title = "Bild und Ton",
    .mode = "Anzeigemodus",
    .modes = {"Fenster", "Vollbild"},
    .resolution = "Aufl\xc3\xb6sung",
    .screen_size = "Bildschirmgr\xc3\xb6\xc3\x9f" "e",
    .frame_rate = "Bildrate",
    .sharpness = "Bildsch\xc3\xa4rfe",
    .sharpnesses = {"Normal", "Gesch\xc3\xa4rft", "Extra scharf"},
    .fov = "Sichtfeld",
    .textures = "Texturfilterung",
    .game_textures = "Wie im Spiel",
    .motion_blur = "Bewegungsunsch\xc3\xa4rfe",
    .fringes = "Farbs\xc3\xa4ume",
};

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

// A dropdown of `labels`. Returns true when the choice changed.
bool dropdown(const char* id, const std::vector<std::string>& labels, int& index) {
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

// The rows' geometry, logical pixels.
constexpr float row_h = 44;
constexpr float control_x = 230;  // from the window's left edge
constexpr float control_w = 226;

class PicturePage : public Page {
public:
    const char* title(const Context& ctx) const override { return text(ctx).title; }

    Action draw(const Context& ctx, float height) override {
        const bool first_frame = !loaded_;
        load(ctx);
        const Text& t = text(ctx);
        const float s = ctx.scale;
        const PictureChoices was = now_;
        ImGui::SetCursorPosX(0);
        // All rows fit at the design's scale; at a small one the child scrolls, and gamepad
        // navigation scrolls it to the focused row.
        ImGui::BeginChild("picture", ImVec2(ImGui::GetWindowWidth(), height));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);  // the band of the focused row goes behind it
        dl->ChannelsSetCurrent(1);
        row_ = 0;
        focus_first_ = first_frame;

        int mode = now_.fullscreen ? 1 : 0;
        if (row(t.mode, s, [&] { return dropdown("##mode", {t.modes[0], t.modes[1]}, mode); })) {
            now_.fullscreen = mode == 1;
        }
        if (now_.fullscreen) {
            row(t.resolution, s, [&] {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", t.screen_size);
                return false;
            });
        } else {
            std::vector<std::string> labels;
            int index = 0;
            for (const Size sz : sizes_) {
                index = sz == now_.size ? static_cast<int>(labels.size()) : index;
                labels.push_back(size_label(sz));
            }
            if (row(t.resolution, s, [&] { return dropdown("##size", labels, index); })) {
                now_.size = sizes_[static_cast<size_t>(index)];
            }
        }

        std::vector<std::string> rates;
        int rate = 0;
        for (const unsigned r : common::frame_rates) {
            rate = r == now_.frame_rate ? static_cast<int>(rates.size()) : rate;
            rates.push_back(std::format("{} FPS", r));
        }
        if (row(t.frame_rate, s, [&] { return dropdown("##rate", rates, rate); })) {
            now_.frame_rate = common::frame_rates[static_cast<size_t>(rate)];
        }

        int look = static_cast<int>(now_.sharpness);
        if (row(t.sharpness, s, [&] {
                return dropdown("##look", {t.sharpnesses[0], t.sharpnesses[1], t.sharpnesses[2]},
                                look);
            })) {
            now_.sharpness = static_cast<Sharpness>(look);
        }

        // Whole degrees; a fractional angle in the file stays until the slider moves.
        int degrees = static_cast<int>(std::lround(now_.fov));
        if (row(t.fov, s, [&] {
                const int min = static_cast<int>(common::min_fov);
                const int max = static_cast<int>(common::max_fov);
                // The angle stands right of the slider, where the grab can't cover it.
                const float value_w = 48 * s;
                ImGui::SetNextItemWidth(control_w * s - value_w);
                bool moved =
                    ImGui::SliderInt("##fov", &degrees, min, max, "", ImGuiSliderFlags_AlwaysClamp);
                const std::string value = std::format("{}\xC2\xB0", degrees);
                const ImVec2 at = ImGui::GetItemRectMax();
                const float text_w = ImGui::CalcTextSize(value.c_str()).x;
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(at.x + value_w - text_w, at.y - ImGui::GetFrameHeight() * 0.5f -
                                                        ImGui::GetTextLineHeight() * 0.5f),
                    colour::text, value.c_str());
                // As in a game's settings: left and right move it while it has the focus,
                // without pressing A first.
                if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
                    const int step = (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
                                      ImGui::IsKeyPressed(ImGuiKey_RightArrow)) -
                                     (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
                                      ImGui::IsKeyPressed(ImGuiKey_LeftArrow));
                    if (step != 0 && degrees + step >= min && degrees + step <= max) {
                        degrees += step;
                        moved = true;
                    }
                }
                return moved;
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
        if (row(t.textures, s, [&] { return dropdown("##textures", filters, level); })) {
            now_.anisotropy = levels[static_cast<size_t>(level)];
        }

        row(t.motion_blur, s, [&] { return ImGui::Checkbox("##blur", &now_.motion_blur); });
        row(t.fringes, s,
            [&] { return ImGui::Checkbox("##fringes", &now_.chromatic_aberration); });

        ImGui::Dummy(ImVec2(0, 0));  // the cursor below the last row is the child's end
        dl->ChannelsMerge();
        ImGui::EndChild();
        if (now_.fullscreen != was.fullscreen || now_.size != was.size) {
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
    // One row: `label` on the left, the control `draw` puts on the right. The row of the focused
    // or hovered control gets a band behind it and a lighter label. Returns what `draw` returns.
    template <typename Control>
    bool row(const char* label, float s, Control draw) {
        const float y = ImGui::GetCursorPosY();
        const float frame = ImGui::GetFrameHeight();
        const float control_y = y + (row_h * s - frame) * 0.5f;
        ImGui::SetCursorPos(ImVec2(control_x * s, control_y));
        ImGui::SetNextItemWidth(control_w * s);
        if (focus_first_ && row_ == 0) {  // gamepad navigation starts at the first row
            ImGui::SetKeyboardFocusHere();
        }
        const bool changed = draw();
        // Plain text (the resolution in fullscreen) has no ID and never lights up.
        const bool lit = ImGui::GetItemID() != 0 &&
                         (ImGui::IsItemFocused() || ImGui::IsItemHovered() || ImGui::IsItemActive());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 win = ImGui::GetWindowPos();
        const float top = win.y + y - ImGui::GetScrollY();
        if (lit) {
            dl->ChannelsSetCurrent(0);
            const float x0 = win.x + 12 * s;
            const float x1 = win.x + ImGui::GetWindowWidth() - 12 * s;
            const float mid = (x0 + x1) * 0.5f;
            constexpr ImU32 clear = IM_COL32(214, 160, 72, 0);
            constexpr ImU32 band = IM_COL32(214, 160, 72, 46);
            dl->AddRectFilledMultiColor(ImVec2(x0, top), ImVec2(mid, top + row_h * s), clear, band,
                                        band, clear);
            dl->AddRectFilledMultiColor(ImVec2(mid, top), ImVec2(x1, top + row_h * s), band, clear,
                                        clear, band);
            dl->ChannelsSetCurrent(1);
        }
        const float text_h = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(win.x + 24 * s, top + (row_h * s - text_h) * 0.5f),
                    lit ? colour::chosen : colour::text, label);
        ImGui::SetCursorPos(ImVec2(0, y + row_h * s));
        ++row_;
        return changed;
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
    bool focus_first_ = false;
    int row_ = 0;
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
