// The logic behind the Help with problems page (docs/design/launcher.md): which known problem
// applies, the capture of the last run and the problem report with its scrubbing. No SDL or
// ImGui in here, so the unit tests cover it.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/game_folder.hpp"

namespace bb::launcher {

enum class Problem { folder_moved, wrong_version, no_gpu, stopped };

struct ProblemInput {
    bool have_folder = false;  // a game folder is set in the config file or was chosen
    common::FolderCheck folder = common::FolderCheck::ok;  // the result of its check
    bool gpu_ok = true;
    bool last_run_failed = false;
};
bool problem_applies(Problem problem, const ProblemInput& in);
// All four problems, the ones that apply now first, each group in the order of the main
// screen's checks (game folder, GPU, last run).
std::vector<Problem> order_problems(const ProblemInput& in);

// ---- The capture of the last run ------------------------------------------------------------
// The launcher sends the output of bb-run to last_run_log(state) and writes how it ended to
// last_run_status(state), both in the logs folder of the runtime's state directory (user/logs).
std::filesystem::path last_run_log(const std::filesystem::path& state_dir);
std::filesystem::path last_run_status(const std::filesystem::path& state_dir);
// "exit status 3", "killed by signal 11" ... from a waitpid status.
std::string describe_wait_status(int wait_status);
// The last `max_bytes` of a file, from the start of a line; empty when it can't be read.
std::string read_tail(const std::filesystem::path& file, std::size_t max_bytes);

// ---- The report ---------------------------------------------------------------------------
// The last `count` lines of `text`.
std::string last_lines(std::string_view text, std::size_t count);
// Replaces the home directory (and any /home/<name>, /var/home/<name>) with ~, then every whole
// word that equals the user name with <user>.
std::string scrub(std::string_view text, std::string_view home, std::string_view user);

inline constexpr std::size_t report_log_lines = 200;

struct ReportInput {
    std::string launcher_version;
    std::string bb_run;       // "found next to the launcher" or "missing"
    std::string distribution;
    std::string kernel;
    std::string gpu;
    std::string last_exit;    // empty when there was no run
    std::string log;          // the whole captured log, or its tail
    std::string home;
    std::string user;
};
// The text for the clipboard, scrubbed as a whole.
std::string build_report(const ReportInput& in);

}  // namespace bb::launcher
