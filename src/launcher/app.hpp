#pragma once

namespace bb::launcher {

// Runs the launcher until its window closes. Returns the process's exit status. Besides the
// window, a test run can drive it with a script: BB_LAUNCHER_SCRIPT names a file of steps (see
// logic.hpp) that feed drops and gamepad presses, change the config file that Play passes on and
// save the rendered frame as a PNG. It exists for the checks of docs/design/launcher.md and its
// tests; a player never sets it.
int run_launcher();

}  // namespace bb::launcher
