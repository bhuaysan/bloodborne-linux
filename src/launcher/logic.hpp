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
    advanced, controller
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

// ---- The scripted input of a test run (BB_LAUNCHER_SCRIPT, see app.cpp) ----------------------------
// One step per line: `wait N` (frames), `drop PATH`, `press accept|back|up|down|left|right`,
// `shot PATH` (the rendered frame as a PNG), `config PATH` (Play uses this config file from now
// on), `waitgame SECONDS` (waits for the game, then ends it), `nopad` (acts as if no controller
// were connected from now on), `search` (a script with this step holds back the first-start
// search's result until the step, which waits for the search to end and hands the result over)
// and `quit`. Empty lines and lines that start with # are skipped.
enum class StepKind { wait, drop, press, shot, config, waitgame, nopad, search, quit };
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
