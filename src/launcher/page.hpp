// A page of the launcher (Picture and sound, Saves): drawn in the area of the
// button column, under the status line, in the same window. The app draws the Back button below
// it and calls on_close when the page is left.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "launcher/logic.hpp"
#include "launcher/text.hpp"

namespace bb::launcher {

// What the app gives a page. `scale` is the factor from the design's logical pixels (480 x 600)
// to ImGui units; multiply every size by it.
struct Context {
    Lang lang = Lang::en;
    float scale = 1.0f;
    const Strings* text = nullptr;
    std::filesystem::path config_path;  // the config file, the only state of the launcher
    bool game_ready = false;            // a game folder passed its check
    std::filesystem::path game_dir;     // empty unless game_ready
};

class Page {
public:
    virtual ~Page() = default;
    // The page's heading, shown in its first line.
    virtual const char* title(const Context& ctx) const = 0;
    // Draws into the current ImGui window, from the cursor down to `height` pixels.
    virtual void draw(const Context& ctx, float height) = 0;
    // Called once when the player leaves the page (Back, B button). Pages save their changes here.
    virtual void on_close(const Context&) {}
};

// One factory per page, each defined in its page_*.cpp.
std::unique_ptr<Page> make_picture_page();
std::unique_ptr<Page> make_saves_page();

// The first-start search (page_find.cpp): its strings, and the list of several folders that
// passed the check. `chosen` is called from draw() when the player picks one.
struct FindText {
    const char* title;           // the list's heading
    const char* hint;            // above the list
    const char* searching;       // status line while the search runs
    const char* found;           // before the folder under the status line
    const char* choose_another;  // the link next to it
};
const FindText& find_text(Lang lang);
std::unique_ptr<Page> make_find_page(std::vector<std::filesystem::path> folders,
                                     std::function<void(const std::filesystem::path&)> chosen);

}  // namespace bb::launcher
