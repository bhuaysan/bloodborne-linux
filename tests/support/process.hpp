// Helpers for tests that run one of our binaries through the shell.
#pragma once

#include <sys/wait.h>

#include <cstdio>
#include <string>

namespace bb::test {

struct Result {
    int status;          // the exit status, -1 when the process didn't exit normally
    std::string output;  // what the command wrote to its standard output
};

// Runs `command` through /bin/sh and captures its standard output. Add " 2>&1" to the command to
// capture standard error as well.
inline Result run(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return {-1, "popen failed"};
    }
    std::string output;
    char buffer[4096];
    while (const std::size_t n = std::fread(buffer, 1, sizeof buffer, pipe)) {
        output.append(buffer, n);
    }
    const int status = pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

// `s` as a single-quoted shell word.
inline std::string quote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

}  // namespace bb::test
