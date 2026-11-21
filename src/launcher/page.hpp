// A page of the launcher (Settings and the pages under it, Saves): it fills the window under its
// title, in the same window. The app draws the frame (title, lines, Back entry), opens the page
// an action asks for and calls on_close when the page is left.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/runtime_dirs.hpp"
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
    std::string version;                // the game's version, "01.09"; empty unless game_ready
    std::string notice;                 // the last refused folder or other problem; may be empty
    bool controller = false;            // a controller is connected (or a script plays one)
    bool focus_first = false;           // the page was just opened or shown again
};

class Page {
public:
    virtual ~Page() = default;
    // The page's heading, shown in its first line.
    virtual const char* title(const Context& ctx) const = 0;
    // Draws into the current ImGui window, from the cursor down to `height` pixels. Returns
    // what the player chose there for the app to do (open a page or the folder dialog).
    virtual Action draw(const Context& ctx, float height) = 0;
    // Called once when the player leaves the page (Back, B button). Pages save their changes here.
    virtual void on_close(const Context&) {}
};

// One factory per page, each defined in its page_*.cpp.
std::unique_ptr<Page> make_settings_page();
std::unique_ptr<Page> make_game_folder_page();
std::unique_ptr<Page> make_controller_page();
std::unique_ptr<Page> make_picture_page();
std::unique_ptr<Page> make_advanced_page();
std::unique_ptr<Page> make_saves_page();

// The strings of the Picture and sound and the Advanced page in `lang`, for the test that every
// one is there.
std::vector<const char*> picture_strings(Lang lang);
std::vector<const char*> advanced_strings(Lang lang);

// The first-start screen of an AppImage (page_datafolder.cpp): where the game keeps its data. It
// has no Back entry; the app shows it instead of the main screen until a folder is chosen.
struct DataFolderText {
    const char* title;
    const char* body;
    const char* use_folder;  // "Use this folder"
    const char* choose;      // "Choose..."
    const char* dialog_title;
    const char* no_proposal;  // no folder to propose: only Choose is useful
};
const DataFolderText& data_folder_text(Lang lang);
// The player-facing text of a refused folder.
std::string data_folder_problem_text(Lang lang, DataFolderProblem problem);

// Shown when a chosen game folder and the data folder lie inside one another.
std::string game_in_data_folder_text(Lang lang);
// The line on the first-start screen when the data folder chosen before is gone or unusable
// (empty for "not chosen yet"). `folder` is the one the pointer file named.
std::string data_folder_lost_text(Lang lang, common::NoDataFolder::Reason reason,
                                  const std::filesystem::path& folder);

enum class DataFolderChoice { none, use_proposed, choose_other };
// Draws the screen into the current ImGui window. `error` is shown under the folder when not empty.
// `focus_first` moves the gamepad focus to "Use this folder".
DataFolderChoice draw_data_folder_screen(const Context& ctx, const std::filesystem::path& proposed,
                                         const std::string& error, bool focus_first);

// The first-start search (page_find.cpp): its strings, and the list of several folders that
// passed the check. `chosen` is called from draw() when the player picks one.
struct FindText {
    const char* title;           // the list's heading
    const char* hint;            // above the list
    const char* searching;       // status line while the search runs
    const char* choose_another;  // the entry under it
};
const FindText& find_text(Lang lang);
std::unique_ptr<Page> make_find_page(std::vector<std::filesystem::path> folders,
                                     std::function<void(const std::filesystem::path&)> chosen);

}  // namespace bb::launcher
