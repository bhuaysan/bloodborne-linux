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
    style.FramePadding = ImVec2(8, 5);
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
    c[ImGuiCol_CheckMark] = c[ImGuiCol_NavCursor] = c[ImGuiCol_SliderGrab] =
        ImGui::ColorConvertU32ToFloat4(colour::gold);
    c[ImGuiCol_SliderGrabActive] = ImGui::ColorConvertU32ToFloat4(colour::chosen);
    c[ImGuiCol_CheckboxSelectedBg] = rgba(214, 160, 72, 0.22f);
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

int prompt(const char* id, const char* title, const char* text,
           std::initializer_list<const char*> options, float scale) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float height = 200 * scale;
    ImGui::SetNextWindowPos(ImVec2(0, (display.y - height) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(display.x, height));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(8, 7, 6, 245));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    const bool open = ImGui::BeginPopupModal(id, nullptr,
                                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!open) {
        return -1;
    }
    const bool appearing = ImGui::IsWindowAppearing();
    rule(0, scale);
    const float width = display.x - 80 * scale;
    centred_text(title, 26 * scale, colour::gold, width, 19 * scale);
    centred_text(text, 62 * scale, colour::text, width, text_size * scale);
    int chosen = -1;
    const ImVec2 size(200 * scale, 40 * scale);
    const auto count = static_cast<float>(options.size());
    ImGui::SetCursorPos(ImVec2((display.x - count * size.x) * 0.5f, 132 * scale));
    int i = 0;
    for (const char* option : options) {
        if (i > 0) {
            ImGui::SameLine(0, 0);
        }
        if (menu_entry(option, size, 17.5f * scale, appearing && i == 0)) {
            chosen = i;
        }
        ++i;
    }
    rule(height / scale - 1, scale);
    if (!appearing && chosen < 0 &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))) {
        chosen = static_cast<int>(options.size()) - 1;
    }
    if (chosen >= 0) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return chosen;
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

std::string ellipsize_right(const std::string& text, float max_width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text;
    }
    std::size_t keep = text.size();
    while (keep > 0) {
        --keep;
        while (keep > 0 && (static_cast<unsigned char>(text[keep]) & 0xc0) == 0x80) {
            --keep;  // stay on a UTF-8 character boundary
        }
        const std::string shorter = text.substr(0, keep) + "...";
        if (ImGui::CalcTextSize(shorter.c_str()).x <= max_width) {
            return shorter;
        }
    }
    return "...";
}

}  // namespace bb::launcher
