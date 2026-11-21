// Widgets shared by the main screen and the pages, in the look of the game's system menu
// (docs/design/launcher.md, "Look"). Sizes are logical pixels times `scale`.
#pragma once

#include <initializer_list>
#include <string>

#include <imgui.h>

namespace bb::launcher {

// The colours of the design.
namespace colour {
inline constexpr ImU32 background = IM_COL32(6, 5, 5, 255);
inline constexpr ImU32 text = IM_COL32(205, 199, 187, 255);
inline constexpr ImU32 chosen = IM_COL32(244, 239, 228, 255);  // the focused entry
inline constexpr ImU32 gold = IM_COL32(201, 165, 95, 255);     // titles
inline constexpr ImU32 dim = IM_COL32(138, 130, 116, 255);     // secondary text
inline constexpr ImU32 line = IM_COL32(205, 196, 178, 128);
inline constexpr ImU32 warning = IM_COL32(210, 162, 78, 255);
inline constexpr ImU32 error = IM_COL32(208, 112, 95, 255);
}  // namespace colour

// Font sizes, logical pixels.
inline constexpr float text_size = 17.0f;
inline constexpr float entry_size = 18.5f;
inline constexpr float first_entry_size = 22.0f;
inline constexpr float hint_size = 13.0f;

// The ImGui colours of the design, for the widgets the pages take from ImGui (combos, lists,
// text fields).
void apply_theme(ImGuiStyle& style);

// A menu entry: centred text without a frame, with a warm glow behind it while it has the focus
// or the mouse. `focus_now` moves the gamepad focus to it. Its ID is the label's, then "##b".
// Returns true when it was pressed.
bool menu_entry(const char* label, ImVec2 size, float font_size, bool focus_now = false);

// The amber glow behind a chosen entry: an ellipse around `centre` that fades out.
void glow(ImDrawList* dl, ImVec2 centre, ImVec2 radius);

// A one-pixel line across the window at `y` (window coordinates) that fades out at both ends.
void rule(float y, float scale);

// `text` centred across the window from `y` down, wrapped to `width`. Returns the y below it.
float centred_text(const char* text, float y, ImU32 colour, float width, float font_size);

// The button hints at the bottom right: Cross with `select` (null: none) and Circle with
// `back` (null: none). Returns true when the mouse pressed the back hint.
bool button_hints(const char* select, const char* back, float scale);

// A prompt over the window: a band with a gold title, a line of text and the options side by
// side, the first one focused. Open it with ImGui::OpenPopup(id). Returns the index of the
// option pressed, the last option for B or Escape, and -1 while it stays open.
int prompt(const char* id, const char* title, const char* text,
           std::initializer_list<const char*> options, float scale);

enum class StatusKind { ok, warning, error };
inline ImU32 status_colour(StatusKind kind) {
    return kind == StatusKind::error ? colour::error
           : kind == StatusKind::warning ? colour::warning : colour::gold;
}

// `text` shortened from the left with "..." until it fits `max_width` (a long folder path keeps
// its end, the part the player recognises).
std::string ellipsize_left(const std::string& text, float max_width);

}  // namespace bb::launcher
