#include "launcher/form.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

#include "launcher/widgets.hpp"

namespace bb::launcher {

Form::Form(const char* id, float height, float scale, bool focus_first)
    : scale_(scale), focus_first_(focus_first) {
    ImGui::SetCursorPosX(0);
    ImGui::BeginChild(id, ImVec2(ImGui::GetWindowWidth(), height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);  // the band of the focused row goes behind it
    dl->ChannelsSetCurrent(1);
}

Form::~Form() {
    ImGui::Dummy(ImVec2(0, 0));  // the cursor below the last row is the child's end
    ImGui::GetWindowDrawList()->ChannelsMerge();
    ImGui::EndChild();
}

bool Form::row(const char* label, const std::function<bool()>& draw) {
    const float s = scale_;
    const float y = ImGui::GetCursorPosY();
    const float frame = ImGui::GetFrameHeight();
    const float control_y = y + (row_h * s - frame) * 0.5f;
    ImGui::SetCursorPos(ImVec2(control_x * s, control_y));
    ImGui::SetNextItemWidth(control_w * s);
    const bool changed = draw();
    if (focus_first_ && row_ == 0) {
        // Gamepad navigation starts at the first row. FocusItem, not SetKeyboardFocusHere: that
        // one activates the item, and a slider then waits for typed text.
        ImGui::FocusItem();
    }
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

float value_column(std::initializer_list<std::string> values, float scale) {
    float w = 0;
    for (const std::string& v : values) {
        w = std::max(w, ImGui::CalcTextSize(v.c_str()).x);
    }
    return w + 12 * scale;
}

namespace {

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

}  // namespace

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

bool line_slider(const char* id, int& value, int min, int max, int mark, int step, float value_w,
                 const std::function<std::string(int)>& label, float s) {
    ImGui::SetNextItemWidth(Form::control_w * s - value_w);
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

}  // namespace bb::launcher
