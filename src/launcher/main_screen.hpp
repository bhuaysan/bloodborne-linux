// The main screen: header, status line and the column of buttons (docs/design/launcher.md).
#pragma once

#include <string>

#include <SDL3/SDL.h>

#include "launcher/logic.hpp"
#include "launcher/page.hpp"
#include "launcher/widgets.hpp"

namespace bb::launcher {

// Logical size of the header; the title picture is cropped to this aspect ratio.
inline constexpr int header_width = 480;
inline constexpr int header_height = 216;

struct MainView {
    MainState state = MainState::no_folder;
    std::string status;
    StatusKind status_kind = StatusKind::warning;
    SDL_Texture* header = nullptr;  // the title picture; null shows the drop target
    bool focus_first = false;       // move the focus to the first button in this frame
};

// Draws the whole window content and returns the action of the button that was pressed.
Action draw_main_screen(const Context& ctx, const MainView& view);

// The header and the status line only, for the pages. Returns the y of the area below them.
float draw_top(const Context& ctx, const MainView& view);

}  // namespace bb::launcher
