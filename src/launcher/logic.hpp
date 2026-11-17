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

// The state of the main screen. The order of the checks is game folder, GPU, last run, controller.
enum class MainState { no_folder, check_failed, ready, no_controller, no_gpu, last_run_failed };

struct StateInput {
    bool have_folder = false;  // a game folder is set in the config file or was just chosen
    bool folder_ok = false;    // and it passed the check
    bool gpu_ok = true;        // the Vulkan loader lists a physical device
    bool controller = true;
    bool last_run_failed = false;
};
MainState select_state(const StateInput& in);

enum class Action { none, set_up, play, picture, saves, help, quit };
// The first button of a state.
Action first_button(MainState state);
// Whether Help with problems is drawn highlighted.
bool help_highlighted(MainState state);
// The header shows the title picture, not the drop target.
bool shows_picture(MainState state);

// What Play runs: `bb_run --config <config>` and nothing else.
std::vector<std::string> bb_run_arguments(const std::filesystem::path& bb_run,
                                          const std::filesystem::path& config);

// ---- The scripted input of a test run (BB_LAUNCHER_SCRIPT, see app.cpp) ----------------------------
// One step per line: `wait N` (frames), `drop PATH`, `press accept|back|up|down|left|right`,
// `shot PATH` (the rendered frame as a PNG), `config PATH` (Play uses this config file from now
// on), `waitgame SECONDS` (waits for the game, then ends it) and `quit`. Empty lines and lines
// that start with # are skipped.
enum class StepKind { wait, drop, press, shot, config, waitgame, quit };
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
