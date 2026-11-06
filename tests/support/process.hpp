// Helpers for tests that run one of our binaries through the shell.
#pragma once

#include <string>

namespace bb::test {

struct Result {
    int status;          // the exit status, -1 when the process didn't exit normally
    std::string output;  // what the command wrote to its standard output
};

// Runs `command` through /bin/sh and captures its standard output. Add " 2>&1" to the command to
// capture standard error as well.
Result run(const std::string& command);

// `s` as a single-quoted shell word.
std::string quote(const std::string& s);

}  // namespace bb::test
