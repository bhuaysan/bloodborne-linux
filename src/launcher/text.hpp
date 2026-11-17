// The strings of the main screen and the words every page shares, in English and German. A page
// keeps its own strings in its own file (page_*.cpp) so that pages don't edit this table.
#pragma once

#include <string>

#include "common/game_folder.hpp"
#include "launcher/logic.hpp"

namespace bb::launcher {

struct Strings {
    const char* play;
    const char* set_up;
    const char* picture_sound;
    const char* saves;
    const char* help;
    const char* quit;
    const char* back;
    const char* status_no_game;
    const char* status_version;       // before the version number
    const char* status_controller;    // "Controller connected"
    const char* status_no_controller;
    const char* status_no_gpu;
    const char* status_last_run_failed;
    const char* drop_hint;
    const char* set_up_first;         // Saves before the game is set up
    const char* dialog_title;
};

const Strings& strings(Lang lang);

// The message of a check for the status line: the check's own text in English, a German text per
// kind in German.
std::string check_message(Lang lang, const common::FolderResult& result);

}  // namespace bb::launcher
