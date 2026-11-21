#include "launcher/run_log.hpp"

#include <sys/wait.h>

#include <cstring>

namespace bb::launcher {

namespace fs = std::filesystem;

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

}  // namespace bb::launcher
