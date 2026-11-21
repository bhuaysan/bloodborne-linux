#include "launcher/widgets.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numbers>
#include <string>

#include <imgui_internal.h>

namespace bb::launcher {

namespace {

ImVec4 rgba(int r, int g, int b, float a) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

}  // namespace

void apply_theme(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(0, 0);
    style.WindowBorderSize = 0;
    style.PopupBorderSize = 1;
    style.FrameRounding = style.PopupRounding = style.ChildRounding = style.WindowRounding = 0;
    style.ScrollbarRounding = style.GrabRounding = 0;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(colour::text);
    c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(colour::dim);
    c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(colour::background);
    c[ImGuiCol_ChildBg] = rgba(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgba(12, 11, 10, 0.98f);
    c[ImGuiCol_Border] = rgba(205, 196, 178, 0.35f);
    c[ImGuiCol_FrameBg] = rgba(205, 196, 178, 0.06f);
    c[ImGuiCol_FrameBgHovered] = rgba(205, 196, 178, 0.12f);
    c[ImGuiCol_FrameBgActive] = rgba(205, 196, 178, 0.16f);
    c[ImGuiCol_Button] = rgba(205, 196, 178, 0.08f);
    c[ImGuiCol_ButtonHovered] = c[ImGuiCol_Header] = rgba(214, 160, 72, 0.28f);
    c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderHovered] = rgba(214, 160, 72, 0.38f);
    c[ImGuiCol_HeaderActive] = rgba(214, 160, 72, 0.48f);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_NavCursor] = ImGui::ColorConvertU32ToFloat4(colour::gold);
    c[ImGuiCol_TextSelectedBg] = rgba(214, 160, 72, 0.35f);
    c[ImGuiCol_ScrollbarBg] = rgba(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgba(205, 196, 178, 0.25f);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = rgba(205, 196, 178, 0.4f);
    c[ImGuiCol_ModalWindowDimBg] = rgba(4, 3, 3, 0.78f);
}

void glow(ImDrawList* dl, ImVec2 centre, ImVec2 radius) {
    // Three rings: amber in the middle, a dimmer brown at half the radius, clear at three
    // quarters. The SDL renderer blends the vertex colours across each triangle.
    constexpr unsigned segments = 40;
    constexpr ImU32 inner = IM_COL32(214, 160, 72, 107);
    constexpr ImU32 middle = IM_COL32(150, 95, 35, 46);
    constexpr ImU32 outer = IM_COL32(150, 95, 35, 0);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(segments * 9, 1 + 2 * segments);
    const auto base = static_cast<unsigned>(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(centre, uv, inner);
    for (const float f : {0.48f, 0.75f}) {
        for (unsigned i = 0; i < segments; ++i) {
            const float a = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / segments;
            dl->PrimWriteVtx(ImVec2(centre.x + std::cos(a) * radius.x * f,
                                    centre.y + std::sin(a) * radius.y * f),
                             uv, f < 0.5f ? middle : outer);
        }
    }
    const auto idx = [&](unsigned i) { dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + i)); };
    for (unsigned i = 0; i < segments; ++i) {
        const unsigned next = (i + 1) % segments;
        idx(0), idx(1 + i), idx(1 + next);
        idx(1 + i), idx(1 + next), idx(1 + segments + next);
        idx(1 + i), idx(1 + segments + next), idx(1 + segments + i);
    }
}

bool menu_entry(const char* label, ImVec2 size, float font_size, bool focus_now) {
    ImGui::PushID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));  // the glow shows the focus
    const bool pressed = ImGui::InvisibleButton("##b", size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    if (focus_now) {  // gamepad navigation starts from this entry
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::GetCurrentContext()->NavCursorVisible = true;  // A works at once, without a D-pad press
    }
    const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused() || ImGui::IsItemActive();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 centre(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    ImFont* font = ImGui::GetFont();
    const ImVec2 text = font->CalcTextSizeA(font_size, size.x, 0, label);
    if (lit) {  // as wide as the label and a margin, within the entry
        glow(dl, centre, ImVec2(std::min(text.x * 0.5f + 2 * size.y, size.x * 0.5f), size.y * 0.5f));
    }
    dl->AddText(font, font_size, ImVec2(centre.x - text.x * 0.5f, centre.y - text.y * 0.5f),
                lit ? colour::chosen : colour::text, label);
    return pressed;
}

void rule(float y, float scale) {
    const ImVec2 win = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();
    const float x0 = win.x + 50 * scale;
    const float x1 = win.x + width - 50 * scale;
    const float fade = (x1 - x0) * 0.12f;
    const float top = win.y + y * scale;
    const float bottom = top + std::max(1.0f, scale);
    constexpr ImU32 clear = colour::line & ~IM_COL32_A_MASK;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(ImVec2(x0, top), ImVec2(x0 + fade, bottom), clear, colour::line,
                                colour::line, clear);
    dl->AddRectFilled(ImVec2(x0 + fade, top), ImVec2(x1 - fade, bottom), colour::line);
    dl->AddRectFilledMultiColor(ImVec2(x1 - fade, top), ImVec2(x1, bottom), colour::line, clear,
                                clear, colour::line);
}

float centred_text(const char* text, float y, ImU32 colour, float width, float font_size) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 win = ImGui::GetWindowPos();
    const float window_width = ImGui::GetWindowWidth();
    // Line by line, so that every line of a wrapped text is centred.
    const char* line = text;
    const char* end = text + std::char_traits<char>::length(text);
    while (line < end) {
        const char* stop = font->CalcWordWrapPosition(font_size, line, end, width);
        if (stop == line) {
            ++stop;  // a word wider than the line still moves on
        }
        const char* newline = std::find(line, stop, '\n');
        stop = newline < stop ? newline : stop;
        const ImVec2 size = font->CalcTextSizeA(font_size, FLT_MAX, 0, line, stop);
        ImGui::GetWindowDrawList()->AddText(font, font_size,
                                            ImVec2(win.x + (window_width - size.x) * 0.5f, win.y + y),
                                            colour, line, stop);
        y += font_size * 1.35f;
        line = stop;
        while (line < end && (*line == ' ' || *line == '\n')) {
            ++line;
        }
    }
    return y;
}

bool button_hints(const char* select, const char* back, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = hint_size * scale;
    const float r = 7.5f * scale;
    const ImVec2 win = ImGui::GetWindowPos();
    float x = win.x + ImGui::GetWindowWidth() - 18 * scale;
    const float cy = win.y + ImGui::GetWindowHeight() - 20 * scale;
    bool back_pressed = false;
    // Right to left: Circle (back), then Cross (select).
    for (int i = 0; i < 2; ++i) {
        const char* label = i == 0 ? back : select;
        if (label == nullptr) {
            continue;
        }
        const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0, label);
        x -= text.x;
        dl->AddText(font, size, ImVec2(x, cy - text.y * 0.5f), colour::dim, label);
        const ImVec2 c(x - 5 * scale - r, cy);
        dl->AddCircle(c, r, IM_COL32(160, 150, 134, 255), 0, scale);
        const float k = r * 0.45f;
        if (i == 0) {
            dl->AddCircle(c, k, colour::chosen, 0, scale);
            // The mouse can go back here too: the hint is its Back button.
            const ImVec2 min(c.x - r, cy - r);
            ImGui::SetCursorScreenPos(min);
            back_pressed = ImGui::InvisibleButton("##back", ImVec2(x + text.x - min.x, 2 * r));
        } else {
            dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), colour::chosen, scale);
            dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), colour::chosen, scale);
        }
        x = c.x - r - 14 * scale;
    }
    return back_pressed;
}

std::string ellipsize_left(const std::string& text, float max_width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text;
    }
    std::size_t cut = 0;
    while (cut < text.size()) {
        ++cut;
        while (cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) {
            ++cut;  // stay on a UTF-8 character boundary
        }
        const std::string shorter = "..." + text.substr(cut);
        if (ImGui::CalcTextSize(shorter.c_str()).x <= max_width) {
            return shorter;
        }
    }
    return "...";
}

}  // namespace bb::launcher
