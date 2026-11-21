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

// A dropdown of `labels`. The closed dropdown shows the choice shortened to its width; the list
// shows the whole names. Returns true when the choice changed.
bool dropdown(const char* id, const std::vector<std::string>& labels, int& index) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float room = ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - 2 * style.FramePadding.x;
    const std::string preview = ellipsize_right(labels[static_cast<size_t>(index)], room);
    bool changed = false;
    if (ImGui::BeginCombo(id, preview.c_str())) {
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

// A dropdown of `names` with `first` (the choice that writes no key) at the top. A name that is
// set but no longer listed (a card or monitor that is gone) stays as the last entry.
bool name_dropdown(const char* id, const char* first, std::vector<std::string> names,
                   std::string& value) {
    if (!value.empty() && std::find(names.begin(), names.end(), value) == names.end()) {
        names.push_back(value);
    }
    std::vector<std::string> labels{first};
    labels.insert(labels.end(), names.begin(), names.end());
    int index = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        index = names[i] == value ? static_cast<int>(i) + 1 : index;
    }
    if (!dropdown(id, labels, index)) {
        return false;
    }
    value = index == 0 ? std::string() : names[static_cast<size_t>(index) - 1];
    return true;
}

// The look of a slider over the last item, ImGui's SliderInt: a thin line with a diamond at
// `value` and a small tick at `mark` (the default). The diamond sits where ImGui puts its grab,
// so clicks and drags land under it.
void draw_line_slider(int value, int min, int max, int mark, float s, bool lit) {
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    constexpr float padding = 2.0f;  // ImGui's grab padding
    const float size = hi.x - lo.x - 2 * padding;
    const float range = static_cast<float>(max - min);
    const float grab = std::min(std::max(size / (range + 1), ImGui::GetStyle().GrabMinSize), size);
    const float x0 = lo.x + padding + grab * 0.5f;
    const float x1 = hi.x - padding - grab * 0.5f;
    const auto at = [&](int v) { return x0 + (x1 - x0) * static_cast<float>(v - min) / range; };
    const float y = std::round((lo.y + hi.y) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 line = lit ? IM_COL32(205, 196, 178, 170) : colour::line;
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + std::max(1.0f, s)), line);
    dl->AddRectFilled(ImVec2(at(mark), y - 4 * s), ImVec2(at(mark) + std::max(1.0f, s), y + 5 * s),
                      line);
    const float r = 6.5f * s;
    const float x = at(value);
    const ImVec2 top(x, y - r), right(x + r, y), bottom(x, y + r), left(x - r, y);
    dl->AddQuadFilled(top, right, bottom, left, lit ? colour::chosen : colour::gold);
    dl->AddQuad(top, right, bottom, left, colour::background, s);
}

// A slider of whole numbers from `min` to `max` in the row's control space, `width` wide: the
// line slider, with `label(value)` right-aligned in the last `value_w` of it, where the diamond
// can't cover it. Left and right move it by `step` while it has the focus, without pressing A
// first, as in a game's settings. Returns true when it moved.
bool line_slider(const char* id, int& value, int min, int max, int mark, int step, float width,
                 float value_w, const std::function<std::string(int)>& label, float s) {
    ImGui::SetNextItemWidth(width - value_w);
    // ImGui's slider does the input; its frame and grab are hidden, and a line with a diamond
    // stands in for them.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    bool moved = ImGui::SliderInt(id, &value, min, max, "", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor(6);
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        const int dir = (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
                         ImGui::IsKeyPressed(ImGuiKey_RightArrow)) -
                        (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
                         ImGui::IsKeyPressed(ImGuiKey_LeftArrow));
        const int next = std::clamp(value + dir * step, min, max);
        moved = moved || next != value;
        value = next;
    }
    draw_line_slider(value, min, max, mark, s,
                     ImGui::IsItemFocused() || ImGui::IsItemHovered() || ImGui::IsItemActive());
    const std::string shown = label(value);
    const ImVec2 at = ImGui::GetItemRectMax();
    const float text_w = ImGui::CalcTextSize(shown.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(at.x + value_w - text_w,
               at.y - ImGui::GetFrameHeight() * 0.5f - ImGui::GetTextLineHeight() * 0.5f),
        colour::text, shown.c_str());
    return moved;
}

// The rows' geometry, logical pixels: eleven rows fill the page's 448.
constexpr float row_h = 40;
constexpr float control_x = 230;  // from the window's left edge
constexpr float control_w = 226;

class PicturePage : public Page {
public:
    const char* title(const Context& ctx) const override { return text(ctx.lang).title; }

    Action draw(const Context& ctx, float height) override {
        const bool first_frame = !loaded_;
        load(ctx);
        const Text& t = text(ctx.lang);
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
        row(t.display, s,
            [&] { return name_dropdown("##display", t.main_display, displays_, now_.display); });
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

        int sync = static_cast<int>(std::find(std::begin(sync_modes), std::end(sync_modes),
                                              now_.present_mode) -
                                    std::begin(sync_modes));
        if (row(t.sync, s,
                [&] { return dropdown("##sync", {t.syncs[0], t.syncs[1], t.syncs[2]}, sync); })) {
            now_.present_mode = sync_modes[sync];
        }
        row(t.gpu, s, [&] { return name_dropdown("##gpu", t.automatic, gpus_, now_.device); });

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
        float value_w = 0;
        for (const std::string& v : {angle(game), percent(full_effect), std::string(t.off)}) {
            value_w = std::max(value_w, ImGui::CalcTextSize(v.c_str()).x);
        }
        value_w += 12 * s;

        row(t.sharpness, s, [&] {
            return line_slider("##sharp", now_.sharpness, sharpness_off, max_sharpness,
                               sharpness_off, 1, control_w * s, value_w, [&](int v) {
                                   return v == sharpness_off ? std::string(t.off)
                                                             : std::to_string(v);
                               }, s);
        });

        if (row(t.fov, s, [&] {
                return line_slider("##fov", degrees, static_cast<int>(common::min_fov),
                                   static_cast<int>(common::max_fov), game, 1, control_w * s,
                                   value_w, angle, s);
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

        row(t.motion_blur, s, [&] {
            return line_slider("##blur", now_.motion_blur, 0, full_effect, full_effect, 5,
                               control_w * s, value_w, percent, s);
        });
        row(t.fringes, s, [&] {
            return line_slider("##fringes", now_.chromatic_aberration, 0, full_effect,
                               full_effect, 5, control_w * s, value_w, percent, s);
        });

        ImGui::Dummy(ImVec2(0, 0));  // the cursor below the last row is the child's end
        dl->ChannelsMerge();
        ImGui::EndChild();
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
        displays_ = display_names();
        gpus_ = vulkan_gpu_names();
        display_ = display_size(now_.display);
        sizes_ = resolution_choices(display_, now_.size);
    }

    bool loaded_ = false;
    bool focus_first_ = false;
    int row_ = 0;
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
