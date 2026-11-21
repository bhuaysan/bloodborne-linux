#include "launcher/main_screen.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "common/version.hpp"

namespace bb::launcher {

namespace {

constexpr float line_y = 252;    // the line under the header
constexpr float bottom_y = 560;  // the line above a page's Back entry
constexpr float entry_h = 46;
constexpr float first_entry_h = 54;
constexpr float entry_w = 432;

void draw_header(const Context& ctx, const MainView& view) {
    const float s = ctx.scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + header_width * s, p0.y + header_height * s);
    if (view.header == nullptr) {
        const float h = ImGui::GetFont()->CalcTextSizeA(text_size * s, FLT_MAX, 0, ctx.text->drop_hint).y;
        centred_text(ctx.text->drop_hint, (line_y * s - h) * 0.5f, colour::dim, entry_w * s,
                     text_size * s);
        return;
    }
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(view.header)), p0, p1);
    // The picture fades into the window's black.
    constexpr ImU32 clear = colour::background & ~IM_COL32_A_MASK;
    constexpr ImU32 dark = (colour::background & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 204);
    const float fade = p0.y + header_height * 0.45f * s;
    const float darker = p0.y + header_height * 0.82f * s;
    dl->AddRectFilledMultiColor(ImVec2(p0.x, fade), ImVec2(p1.x, darker), clear, clear, dark, dark);
    dl->AddRectFilledMultiColor(ImVec2(p0.x, darker), p1, dark, dark, colour::background,
                                colour::background);
}

// The build's version in the bottom right corner, small and dim, for bug reports.
void draw_version(const Context& ctx) {
    const float size = 13 * ctx.scale;
    const std::string_view version = bb::common::build_version();
    const char* const end = version.data() + version.size();
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, version.data(), end);
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 window = ImGui::GetWindowSize();
    const ImVec2 at(p0.x + window.x - extent.x - 10 * ctx.scale,
                    p0.y + window.y - extent.y - 8 * ctx.scale);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size, at, colour::dim, version.data(), end);
}

}  // namespace

float draw_top(const Context& ctx, const MainView& view) {
    const float s = ctx.scale;
    draw_header(ctx, view);
    rule(line_y, s);
    if (shows_picture(view.state)) {
        return 306 * s;
    }
    const float below = centred_text(view.status.c_str(), 266 * s, status_colour(view.status_kind),
                                     entry_w * s, text_size * s);
    return std::max(300 * s, below + 10 * s);
}

Action draw_main_screen(const Context& ctx, const MainView& view) {
    const float s = ctx.scale;
    const float top = draw_top(ctx, view);
    const Strings& t = *ctx.text;
    const Action first = first_button(view.state);
    const std::vector<std::pair<Action, const char*>> entries{
        {first, first == Action::play ? t.play : t.set_up},
        {Action::settings, t.settings},
        {Action::saves, t.saves},
        {Action::quit, t.quit}};
    Action result = Action::none;
    float y = top;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool primary = i == 0;
        const float h = (primary ? first_entry_h : entry_h) * s;
        ImGui::SetCursorPos(ImVec2((header_width - entry_w) * 0.5f * s, y));
        if (menu_entry(entries[i].second, ImVec2(entry_w * s, h),
                       (primary ? first_entry_size : entry_size) * s,
                       primary && view.focus_first)) {
            result = entries[i].first;
        }
        y += h;
    }
    draw_version(ctx);
    return result;
}

bool draw_page_frame(const Context& ctx, const char* title) {
    const float s = ctx.scale;
    rule(22, s);
    centred_text(title, 36 * s, colour::gold, entry_w * s, 19 * s);
    rule(76, s);
    rule(bottom_y, s);
    // The launcher is for the mouse first: Back is an entry of its own (B and Escape work too).
    const ImVec2 size(200 * s, 34 * s);
    ImGui::SetCursorPos(ImVec2((header_width * s - size.x) * 0.5f, (bottom_y + 4) * s));
    return menu_entry(ctx.text->back, size, entry_size * s);
}

}  // namespace bb::launcher
