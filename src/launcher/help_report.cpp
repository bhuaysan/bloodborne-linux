#include "launcher/help_report.hpp"

#include <sys/wait.h>

#include <algorithm>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iterator>

namespace bb::launcher {

namespace fs = std::filesystem;

bool problem_applies(Problem problem, const ProblemInput& in) {
    using common::FolderCheck;
    const bool folder_bad = in.have_folder && in.folder != FolderCheck::ok;
    switch (problem) {
    case Problem::folder_moved:
        return folder_bad && in.folder != FolderCheck::other_title &&
               in.folder != FolderCheck::missing_update && in.folder != FolderCheck::unknown_eboot;
    case Problem::wrong_version:
        return folder_bad && (in.folder == FolderCheck::other_title ||
                              in.folder == FolderCheck::missing_update ||
                              in.folder == FolderCheck::unknown_eboot);
    case Problem::no_gpu: return !in.gpu_ok;
    case Problem::stopped: return in.last_run_failed;
    }
    return false;
}

std::vector<Problem> order_problems(const ProblemInput& in) {
    std::vector<Problem> all{Problem::folder_moved, Problem::wrong_version, Problem::no_gpu,
                             Problem::stopped};
    std::ranges::stable_partition(all, [&](Problem p) { return problem_applies(p, in); });
    return all;
}

fs::path last_run_log(const fs::path& state_dir) {
    return state_dir / "logs" / "launcher-last-run.log";
}

fs::path last_run_status(const fs::path& state_dir) {
    return state_dir / "logs" / "launcher-last-run.status";
}

std::string describe_wait_status(int wait_status) {
    if (WIFEXITED(wait_status)) {
        return "exit status " + std::to_string(WEXITSTATUS(wait_status));
    }
    if (WIFSIGNALED(wait_status)) {
        const int sig = WTERMSIG(wait_status);
        const char* name = strsignal(sig);
        return "killed by signal " + std::to_string(sig) + (name ? std::string(" (") + name + ")" : "");
    }
    return "unknown";
}

std::string read_tail(const fs::path& file, std::size_t max_bytes) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = static_cast<std::size_t>(in.tellg());
    const std::size_t from = size > max_bytes ? size - max_bytes : 0;
    in.seekg(static_cast<std::streamoff>(from));
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (from > 0) {  // start at a line boundary
        const auto nl = text.find('\n');
        text.erase(0, nl == std::string::npos ? text.size() : nl + 1);
    }
    return text;
}

std::string last_lines(std::string_view text, std::size_t count) {
    if (text.ends_with('\n')) {
        text.remove_suffix(1);
    }
    if (count == 0 || text.empty()) {
        return {};
    }
    std::size_t start = text.size();
    std::size_t found = 0;
    while (start > 0) {
        const auto nl = text.rfind('\n', start - 1);
        if (nl == std::string_view::npos) {
            start = 0;
            break;
        }
        if (++found == count) {
            start = nl + 1;
            break;
        }
        start = nl;
        if (start == 0) {
            break;
        }
    }
    return std::string(text.substr(start)) + "\n";
}

namespace {

bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
}

bool word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Replaces every occurrence of `needle` that has no `is_part` character right before it or right
// after it.
std::string replace_bounded(const std::string& text, std::string_view needle,
                            std::string_view with, bool (*is_part)(char)) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const bool here = text.compare(i, needle.size(), needle) == 0 &&
                          (i == 0 || !is_part(text[i - 1])) &&
                          (i + needle.size() == text.size() || !is_part(text[i + needle.size()]));
        if (here) {
            out += with;
            i += needle.size();
        } else {
            out += text[i++];
        }
    }
    return out;
}

}  // namespace

std::string scrub(std::string_view text, std::string_view home, std::string_view user) {
    std::string out(text);
    if (home.size() > 1) {
        while (home.size() > 1 && home.ends_with('/')) {
            home.remove_suffix(1);
        }
        out = replace_bounded(out, home, "~", name_char);
    }
    // Any other home directory: /home/<name> and /var/home/<name> (immutable distributions).
    std::string result;
    std::size_t i = 0;
    while (i < out.size()) {
        if (out.compare(i, 6, "/home/") == 0 && i + 6 < out.size() && name_char(out[i + 6])) {
            if (result.ends_with("/var")) {
                result.resize(result.size() - 4);
            }
            std::size_t end = i + 6;
            while (end < out.size() && name_char(out[end])) {
                ++end;
            }
            result += '~';
            i = end;
        } else {
            result += out[i++];
        }
    }
    out = std::move(result);
    if (!user.empty() && user != "root") {
        out = replace_bounded(out, user, "<user>", word_char);
    }
    return out;
}

std::string build_report(const ReportInput& in) {
    std::string text = "bloodborne-linux problem report\n";
    text += "launcher version: " + in.launcher_version + "\n";
    text += "bb-run: " + in.bb_run + "\n";
    text += "distribution: " + in.distribution + "\n";
    text += "kernel: " + in.kernel + "\n";
    text += "GPU: " + in.gpu + "\n";
    text += "last run: " + (in.last_exit.empty() ? std::string("no run yet") : in.last_exit) + "\n";
    const std::string log = last_lines(in.log, report_log_lines);
    text += "\nlast lines of the log:\n" + (log.empty() ? std::string("(no log)\n") : log);
    return scrub(text, in.home, in.user);
}

}  // namespace bb::launcher
