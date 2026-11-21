// The main screen and the frame of the pages (docs/design/launcher.md, "Layout").
#pragma once

#include <string>

#include <SDL3/SDL.h>

#include "launcher/logic.hpp"
#include "launcher/page.hpp"
#include "launcher/widgets.hpp"

namespace bb::launcher {

// Logical size of the header; the title picture is cropped to this aspect ratio.
inline constexpr int header_width = 480;
inline constexpr int header_height = 262;
// A page's content lies between these, in logical pixels.
inline constexpr float page_top = 100;
inline constexpr float page_bottom = 548;

struct MainView {
    MainState state = MainState::no_folder;
    std::string status;  // shown only without a game
    StatusKind status_kind = StatusKind::warning;
    SDL_Texture* header = nullptr;  // the title picture; null shows the drop hint
    bool focus_first = false;       // move the focus to the first entry in this frame
};

// Draws the whole window content and returns the action of the entry that was pressed.
Action draw_main_screen(const Context& ctx, const MainView& view);

// The header, the line under it and, without a game, the status. Returns the y below them in
// ImGui units: the status text wraps and pushes the menu down.
float draw_top(const Context& ctx, const MainView& view);

// A page's frame: its gold title between two lines, and a line with the Back entry under it.
// Returns true when Back was pressed.
bool draw_page_frame(const Context& ctx, const char* title);

}  // namespace bb::launcher
