// The launcher's pure logic (docs/design/launcher.md): which state the main screen is in, the
// header crop, the language, the bb-run command line and the scripted input of the test run.
// No SDL or ImGui in here, so the unit tests cover all of it.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bb::launcher {

enum class Lang { en, de };

// The language of a locale name such as "de_DE.UTF-8" or "de:en"; English for everything else.
Lang choose_language(std::string_view locale);
// From LC_ALL, LC_MESSAGES, LANGUAGE and LANG, the first one that is set.
Lang language_from_environment();

// "01.09" to "1.09". Anything that is not MAJOR.MINOR stays as it is.
std::string display_version(std::string_view app_version);

struct CropRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
// The largest rectangle of the aspect ratio target_w : target_h that fits the image, around its
// centre. An empty rectangle for sizes of zero or less.
CropRect crop_around_centre(int image_w, int image_h, int target_w, int target_h);

// The state of the main screen: only the game folder changes it. Other problems show as a
// prompt when they matter (see before_play).
enum class MainState { no_folder, check_failed, ready };

struct StateInput {
    bool have_folder = false;  // a game folder is set in the config file or was just chosen
    bool folder_ok = false;    // and it passed the check
};
MainState select_state(const StateInput& in);

// set_up and choose_another open the folder dialog; settings, game_folder, picture, controller
// and saves open a page; play_anyway is Play after the no-controller prompt.
enum class Action {
    none, set_up, play, play_anyway, settings, saves, quit, choose_another, game_folder, picture,
    advanced, controller, change_data, data_default
};
// The first button of a state.
Action first_button(MainState state);
// The header shows the title picture, not the drop target.
bool shows_picture(MainState state);

// The prompts over the window. Play checks the GPU, then the controller; a failed run and a
// notice while the game is set up come up on their own.
enum class Prompt { none, no_gpu, no_controller, run_failed, notice };
Prompt before_play(bool gpu_ok, bool controller);

// What Play runs: `bb_run --config <config>` and nothing else.
std::vector<std::string> bb_run_arguments(const std::filesystem::path& bb_run,
                                          const std::filesystem::path& config);

// ---- The AppImage's data folder (docs/design/launcher.md, "One portable folder") -------------------
inline constexpr const char* data_folder_name = "Bloodborne-Linux-data";

// True when `folder` or one of its parents holds an eboot.bin: it is a game folder or lies inside
// one. The folder need not exist.
bool inside_game_folder(const std::filesystem::path& folder);

// The folder the first-start screen proposes: Bloodborne-Linux-data in the directory of the
// AppImage file when that directory can be written (`appimage_dir_writable`), else in `home`;
// a candidate inside a game folder (inside_game_folder) is skipped. Empty when none is possible
// (no absolute AppImage path and no home).
std::filesystem::path propose_data_folder(const std::filesystem::path& appimage,
                                          std::string_view home, bool appimage_dir_writable);

// The folder where the first-start search looks for the dump next to the program: game/ inside the
// data folder in an AppImage, else game/ next to the executable.
std::filesystem::path game_search_dir(bool appimage, const std::filesystem::path& data_folder,
                                      const std::filesystem::path& executable_dir);

enum class DataFolderProblem { none, not_absolute, cannot_create, not_writable, overlaps, is_game,
                               pointer_failed, inside_game, no_place };
struct DataFolderResult {
    DataFolderProblem problem = DataFolderProblem::none;
    std::string detail;  // the system's or the check's own message, for the log
    bool ok() const { return problem == DataFolderProblem::none; }
};
// Refuses a folder that holds the game's files or lies inside a game folder (is_game, inside_game;
// nothing is created then). Else creates `folder` (mode 0700), checks that it can be written and
// doesn't overlap the source directory, then writes `pointer_file` (DataPlan::pointer_file: the one
// file the runtime reads first, which replaces a broken one). no_place when it is empty.
DataFolderResult adopt_data_folder(const std::filesystem::path& folder,
                                   const std::filesystem::path& pointer_file);

// ---- The scripted input of a test run (BB_LAUNCHER_SCRIPT, see app.cpp) ----------------------------
// One step per line: `wait N` (frames), `drop PATH`, `press accept|back|up|down|left|right`,
// `datafolder PATH` (chooses PATH on the first-start screen, as "Use this folder"
// would), `datadefault` (takes the proposed folder), `movedata PATH` (moves the data folder to
// PATH, as Change on the Advanced page would) and `movedefault` (Back to default), `shot PATH` (the rendered frame as a PNG), `config PATH` (Play uses this config file from now
// on), `waitgame SECONDS` (waits for the game, then ends it), `nopad` (acts as if no controller
// were connected from now on), `search` (a script with this step holds back the first-start
// search's result until the step, which waits for the search to end and hands the result over),
// `otherpad up|down|left|right|accept|back` (plugs in a virtual controller that holds that button,
// as a pad someone uses next to the test run would; the script's own presses must win) and
// `quit`. Empty lines and lines that start with # are skipped.
enum class StepKind { wait, drop, press, shot, config, waitgame, nopad, search, quit, datafolder,
                      datadefault, movedata, movedefault, otherpad };
enum class PadButton { accept, back, up, down, left, right };
struct ScriptStep {
    StepKind kind = StepKind::quit;
    int number = 0;  // frames or seconds
    PadButton button = PadButton::accept;
    std::string text;  // the path
};
// Throws std::invalid_argument with the line number for a line that is not a step.
std::vector<ScriptStep> parse_script(std::string_view text);

}  // namespace bb::launcher
