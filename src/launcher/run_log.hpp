// The capture of the last run (docs/design/launcher.md, "Starting the game"): the launcher sends
// the output of bb-run to last_run_log(state) and writes how it ended to last_run_status(state),
// both in the logs folder of the runtime's state directory (user/logs). bb-run has no log file of
// its own when it stops early, so this is the only trace of such a run.
#pragma once

#include <filesystem>
#include <string>

namespace bb::launcher {

std::filesystem::path last_run_log(const std::filesystem::path& state_dir);
std::filesystem::path last_run_status(const std::filesystem::path& state_dir);
// "exit status 3", "killed by signal 11 (Segmentation fault)" ... from a waitpid status.
std::string describe_wait_status(int wait_status);

}  // namespace bb::launcher
