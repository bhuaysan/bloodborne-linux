#include "launcher/main_screen.hpp"

#include <cstdint>
#include <vector>

#include <imgui.h>

namespace bb::launcher {

namespace {

constexpr float margin = 24;
constexpr float status_height = 40;

void draw_header(const MainView& view, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + header_width * scale, p0.y + header_height * scale);
    if (view.header != nullptr) {
        dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(view.header)), p0, p1);
        return;
    }
    // The drop target: a dashed frame and the hint in its middle.
    dl->AddRectFilled(p0, p1, IM_COL32(26, 26, 32, 255));
    const float inset = 16 * scale;
    const float dash = 10 * scale;
    const ImU32 line = IM_COL32(150, 150, 165, 255);
    const ImVec2 a(p0.x + inset, p0.y + inset);
    const ImVec2 b(p1.x - inset, p1.y - inset);
    for (float x = a.x; x < b.x; x += dash * 2) {
        const float e = x + dash < b.x ? x + dash : b.x;
        dl->AddLine(ImVec2(x, a.y), ImVec2(e, a.y), line, 2);
        dl->AddLine(ImVec2(x, b.y), ImVec2(e, b.y), line, 2);
    }
    for (float y = a.y; y < b.y; y += dash * 2) {
        const float e = y + dash < b.y ? y + dash : b.y;
        dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, e), line, 2);
        dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, e), line, 2);
    }
}

}  // namespace

float draw_top(const Context& ctx, const MainView& view, bool* choose_another) {
    draw_header(view, ctx.scale);
    ImGui::SetCursorPos(ImVec2(margin * ctx.scale, header_height * ctx.scale));
    if (view.header == nullptr) {
        // The hint, centred in the drop target.
        const ImVec2 size = ImGui::CalcTextSize(ctx.text->drop_hint);
        ImGui::SetCursorPos(ImVec2((header_width * ctx.scale - size.x) * 0.5f,
                                   (header_height * ctx.scale - size.y) * 0.5f));
        ImGui::TextUnformatted(ctx.text->drop_hint);
        ImGui::SetCursorPos(ImVec2(margin * ctx.scale, header_height * ctx.scale));
    }
    ImGui::SetCursorPosY(header_height * ctx.scale + 10 * ctx.scale);
    ImGui::SetCursorPosX(margin * ctx.scale);
    status_line(view.status, view.status_kind, ctx.scale);
    if (choose_another != nullptr && !view.found_folder.empty()) {
        const FindText& f = find_text(ctx.lang);
        ImGui::SetCursorPosX(margin * ctx.scale);
        const float link =
            ImGui::CalcTextSize(f.choose_another).x + 2 * ImGui::GetStyle().FramePadding.x;
        const float width = (header_width - 2 * margin) * ctx.scale - link - 8 * ctx.scale;
        const std::string label = std::string(f.found) + " ";
        ImGui::TextDisabled("%s%s", label.c_str(),
                            ellipsize_left(view.found_folder, width - ImGui::CalcTextSize(label.c_str()).x).c_str());
        ImGui::SameLine(0, 8 * ctx.scale);
        *choose_another = ImGui::SmallButton(f.choose_another);
    }
    // The status text wraps: the area grows with it so the buttons never overlap.
    const float fixed = (header_height + status_height) * ctx.scale;
    const float wrapped = ImGui::GetCursorPosY() + 8 * ctx.scale;
    return wrapped > fixed ? wrapped : fixed;
}

Action draw_main_screen(const Context& ctx, const MainView& view) {
    const float s = ctx.scale;
    bool another = false;
    const float below = draw_top(ctx, view, &another);
    const Strings& t = *ctx.text;
    const auto label = [&](Action a) {
        switch (a) {
        case Action::set_up: return t.set_up;
        case Action::play: return t.play;
        case Action::picture: return t.picture_sound;
        case Action::saves: return t.saves;
        default: return t.quit;
        }
    };
    const Action first = first_button(view.state);
    const std::vector<Action> list{first, Action::picture, Action::saves, Action::quit};
    Action result = another ? Action::choose_another : Action::none;
    ImGui::SetCursorPos(ImVec2(margin * s, below + 8 * s));
    for (std::size_t i = 0; i < list.size(); ++i) {
        const bool primary = i == 0;
        const ImVec2 size((header_width - 2 * margin) * s, (primary ? 64 : 48) * s);
        ImGui::SetCursorPosX(margin * s);
        const bool accent = primary && first == Action::play;
        if (bevel_button(label(list[i]), size, accent, primary && view.focus_first)) {
            result = list[i];
        }
        ImGui::Dummy(ImVec2(0, 4 * s));
    }
    return result;
}

}  // namespace bb::launcher
