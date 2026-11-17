#include "launcher/widgets.hpp"

#include <imgui_internal.h>

namespace bb::launcher {

bool bevel_button(const char* label, ImVec2 size, bool accent, bool focus_now) {
    ImGui::PushID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##b", size, ImGuiButtonFlags_EnableNav);
    if (focus_now) {  // gamepad navigation starts from this button
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::GetCurrentContext()->NavCursorVisible = true;  // A works at once, without a D-pad press
    }
    const bool held = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const float bevel = ImGui::GetFontSize() * 0.12f + 1.0f;
    ImVec4 face = accent ? ImVec4(0.45f, 0.10f, 0.10f, 1) : ImVec4(0.27f, 0.27f, 0.31f, 1);
    if (held) {
        face.x *= 0.8f, face.y *= 0.8f, face.z *= 0.8f;
    } else if (hovered || focused) {
        face.x += 0.08f, face.y += 0.08f, face.z += 0.08f;
    }
    const ImU32 light = IM_COL32(210, 210, 220, 255);
    const ImU32 dark = IM_COL32(20, 20, 24, 255);
    const ImU32 top_left = held ? dark : light;
    const ImU32 bottom_right = held ? light : dark;
    dl->AddRectFilled(pos, max, ImGui::ColorConvertFloat4ToU32(face));
    // The bevel: two edges each, as thin filled quads that meet at the corners.
    dl->AddQuadFilled(pos, ImVec2(max.x, pos.y), ImVec2(max.x - bevel, pos.y + bevel),
                      ImVec2(pos.x + bevel, pos.y + bevel), top_left);
    dl->AddQuadFilled(pos, ImVec2(pos.x + bevel, max.y - bevel), ImVec2(pos.x, max.y),
                      ImVec2(pos.x + bevel, pos.y + bevel), top_left);
    dl->AddQuadFilled(ImVec2(max.x, pos.y), max, ImVec2(max.x - bevel, max.y - bevel),
                      ImVec2(max.x - bevel, pos.y + bevel), bottom_right);
    dl->AddQuadFilled(ImVec2(pos.x, max.y), ImVec2(pos.x + bevel, max.y - bevel),
                      ImVec2(max.x - bevel, max.y - bevel), max, bottom_right);
    if (focused) {
        const float inset = bevel * 2.5f;
        dl->AddRect(ImVec2(pos.x + inset, pos.y + inset), ImVec2(max.x - inset, max.y - inset),
                    IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.0f);
    }
    const ImVec2 text = ImGui::CalcTextSize(label);
    const float nudge = held ? bevel * 0.5f : 0.0f;
    dl->AddText(ImVec2(pos.x + (size.x - text.x) * 0.5f + nudge,
                       pos.y + (size.y - text.y) * 0.5f + nudge),
                IM_COL32(245, 245, 245, 255), label);
    return pressed;
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

void status_line(const std::string& text, StatusKind kind, float scale) {
    ImU32 colour = IM_COL32(110, 200, 120, 255);
    if (kind == StatusKind::warning) {
        colour = IM_COL32(230, 190, 80, 255);
    } else if (kind == StatusKind::error) {
        colour = IM_COL32(230, 90, 80, 255);
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float r = 5.0f * scale;
    const float line = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x + r, pos.y + line * 0.5f), r, colour);
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + r * 3.0f, pos.y));
    ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}

}  // namespace bb::launcher
