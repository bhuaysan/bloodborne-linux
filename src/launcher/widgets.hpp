// Widgets shared by the main screen and the pages.
#pragma once

#include <string>

#include <imgui.h>

namespace bb::launcher {

// A bevelled button (light top and left edges, dark bottom and right edges), as in a late-90s
// autorun menu. `accent` gives it the accent colour (Play, and Help after a failed run).
// `focus_now` moves the gamepad focus to it. Returns true when it was pressed.
bool bevel_button(const char* label, ImVec2 size, bool accent = false, bool focus_now = false);

// `text` shortened from the left with "..." until it fits `max_width` (a long folder path keeps
// its end, the part the player recognises).
std::string ellipsize_left(const std::string& text, float max_width);

enum class StatusKind { ok, warning, error };
// A one-line status with a coloured marker. Wraps to the window's width.
void status_line(const std::string& text, StatusKind kind, float scale);

}  // namespace bb::launcher
