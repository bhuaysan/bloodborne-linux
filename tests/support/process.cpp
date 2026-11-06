#include "support/process.hpp"

#include <sys/wait.h>

#include <cstdio>

namespace bb::test {

Result run(const std::string& command) {
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

std::string quote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

}  // namespace bb::test
